/*
 * nRF52840 BLE Application
 *
 * 機能:
 *   - ADV送信  (Peripheral役割: Androidからの接続を受け入れ)
 *   - Scan     (Central役割: 周囲のBLEデバイスを探索)
 *   - BLE接続  (見つけたデバイスへ接続開始)
 *   - 切断     (接続中デバイスとの切断)
 *
 * ボタン操作 (nRF52840 DK):
 *   BTN1 (SW0): ADV ON/OFF トグル
 *   BTN2 (SW1): Scan ON/OFF トグル
 *   BTN3 (SW2): スキャンで見つけたデバイスへ接続
 *   BTN4 (SW3): 接続中デバイスと切断
 *
 * LED表示:
 *   LED1 (LED0): ADV中に点灯
 *   LED2 (LED1): 接続中に点灯
 *
 * ターゲットボード: nrf52840dk_nrf52840
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_app, LOG_LEVEL_INF);

/* ============================================================
 * デバイス名 & カスタムUUID定義
 * ============================================================ */
#define DEVICE_NAME      CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN  (sizeof(DEVICE_NAME) - 1)

/* カスタムサービス UUID: 12345678-1234-5678-1234-56789abcdef0 */
#define BT_UUID_CUSTOM_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

/* カスタムキャラクタリスティック UUID: 12345678-1234-5678-1234-56789abcdef1 */
#define BT_UUID_CUSTOM_CHAR_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

static struct bt_uuid_128 custom_service_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
static struct bt_uuid_128 custom_char_uuid    = BT_UUID_INIT_128(BT_UUID_CUSTOM_CHAR_VAL);

/* ============================================================
 * GPIO - ボタン & LED (nRF52840 DK)
 *   SW0=P0.11, SW1=P0.12, SW2=P0.24, SW3=P0.25 (全てPORT0)
 *   LED0=P0.13, LED1=P0.14
 * ============================================================ */
static const struct gpio_dt_spec buttons[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios),
};

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
};

static struct gpio_callback btn_cb[ARRAY_SIZE(buttons)];

/* ============================================================
 * BLE状態管理
 * ============================================================ */
static struct bt_conn *default_conn; /* 現在の接続ハンドル */
static bt_addr_le_t   target_addr;  /* 接続先アドレス (Scan結果) */
static bool           target_found;  /* 接続候補デバイスが見つかっているか */
static bool           is_advertising;
static bool           is_scanning;

/* ============================================================
 * GATTサービス定義 (Android↔nRF52840 データ通信用)
 * ============================================================ */
static uint8_t char_value[20] = "Hello from nRF52840!";

/* Android→nRF52840 への通知が有効/無効に切り替わった時のコールバック */
static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);

	LOG_INF("[GATT] Notifications %s", notif_enabled ? "enabled" : "disabled");
}

/* Android から Read されたとき */
static ssize_t on_char_read(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	LOG_INF("[GATT] Read request from Android");

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 char_value, strlen(char_value));
}

/* Android から Write されたとき */
static ssize_t on_char_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len,
			     uint16_t offset, uint8_t flags)
{
	if (offset + len > sizeof(char_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(char_value + offset, buf, len);
	char_value[offset + len] = '\0';

	LOG_INF("[GATT] Received from Android: %s", char_value);

	return len;
}

/* GATTサービス登録 */
BT_GATT_SERVICE_DEFINE(custom_svc,
	BT_GATT_PRIMARY_SERVICE(&custom_service_uuid),
	BT_GATT_CHARACTERISTIC(&custom_char_uuid.uuid,
				BT_GATT_CHRC_READ |
				BT_GATT_CHRC_WRITE |
				BT_GATT_CHRC_NOTIFY,
				BT_GATT_PERM_READ |
				BT_GATT_PERM_WRITE,
				on_char_read, on_char_write, char_value),
	BT_GATT_CCC(ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ============================================================
 * アドバタイジング (Peripheral役割)
 * ============================================================ */

/* ADVパケット: フラグ + デバイス名 */
static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

/* Scan Responseパケット: カスタムサービスUUID */
static const struct bt_data scan_rsp_data[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_CUSTOM_SERVICE_VAL),
};

static void adv_start(void)
{
	int err;

	if (is_advertising) {
		LOG_WRN("[ADV] Already advertising");
		return;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN,
			      adv_data, ARRAY_SIZE(adv_data),
			      scan_rsp_data, ARRAY_SIZE(scan_rsp_data));
	if (err) {
		LOG_ERR("[ADV] Start failed (err %d)", err);
		return;
	}

	is_advertising = true;
	gpio_pin_set_dt(&leds[0], 1);
	LOG_INF("[ADV] Started  name=\"%s\"", DEVICE_NAME);
}

static void adv_stop(void)
{
	if (!is_advertising) {
		return;
	}

	bt_le_adv_stop();
	is_advertising = false;
	gpio_pin_set_dt(&leds[0], 0);
	LOG_INF("[ADV] Stopped");
}

/* ============================================================
 * スキャン (Central役割)
 * ============================================================ */

/* スキャンで見つかったデバイスごとに呼ばれるコールバック */
static void scan_recv_cb(const bt_addr_le_t *addr, int8_t rssi,
			 uint8_t type, struct net_buf_simple *ad)
{
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	LOG_INF("[SCAN] Found: %-30s RSSI:%4d  Type:%u", addr_str, rssi, type);

	/* 最初に見つかったデバイスを接続候補として保存 */
	if (!target_found) {
		bt_addr_le_copy(&target_addr, addr);
		target_found = true;
		LOG_INF("[SCAN] >> Target saved: %s (BTN3で接続)", addr_str);
	}
}

static void scan_start(void)
{
	struct bt_le_scan_param param = {
		.type     = BT_LE_SCAN_TYPE_ACTIVE,
		.options  = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window   = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err;

	if (is_scanning) {
		LOG_WRN("[SCAN] Already scanning");
		return;
	}

	target_found = false;

	err = bt_le_scan_start(&param, scan_recv_cb);
	if (err) {
		LOG_ERR("[SCAN] Start failed (err %d)", err);
		return;
	}

	is_scanning = true;
	LOG_INF("[SCAN] Started (Active)");
}

static void scan_stop(void)
{
	if (!is_scanning) {
		return;
	}

	bt_le_scan_stop();
	is_scanning = false;
	LOG_INF("[SCAN] Stopped");
}

/* ============================================================
 * 接続 / 切断
 * ============================================================ */

static void do_connect(void)
{
	struct bt_conn_le_create_param create_param =
		BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE,
					     BT_GAP_SCAN_FAST_INTERVAL,
					     BT_GAP_SCAN_FAST_INTERVAL);
	struct bt_le_conn_param conn_param = *BT_LE_CONN_PARAM_DEFAULT;
	char addr_str[BT_ADDR_LE_STR_LEN];
	int err;

	if (default_conn) {
		LOG_WRN("[CONN] Already connected");
		return;
	}

	if (!target_found) {
		LOG_WRN("[CONN] No target. BTN2でScanしてください");
		return;
	}

	if (is_scanning) {
		scan_stop();
	}

	bt_addr_le_to_str(&target_addr, addr_str, sizeof(addr_str));
	LOG_INF("[CONN] Connecting to %s ...", addr_str);

	err = bt_conn_le_create(&target_addr, &create_param, &conn_param,
				&default_conn);
	if (err) {
		LOG_ERR("[CONN] Create failed (err %d)", err);
		default_conn = NULL;
		target_found = false;
	}
}

static void do_disconnect(void)
{
	int err;

	if (!default_conn) {
		LOG_WRN("[CONN] Not connected");
		return;
	}

	LOG_INF("[CONN] Disconnecting...");

	err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err) {
		LOG_ERR("[CONN] Disconnect failed (err %d)", err);
	}
}

/* ============================================================
 * 接続コールバック
 * ============================================================ */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_ERR("[CONN] Failed: %s  err=0x%02x", addr, err);

		if (default_conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;
		}
		/* 接続失敗後はアドバタイジングを再開 */
		adv_start();
		return;
	}

	LOG_INF("[CONN] Connected: %s", addr);

	/*
	 * Peripheral (ADV) 経由で接続された場合は default_conn が NULL。
	 * Central (bt_conn_le_create) 経由では既に設定済み。
	 */
	if (!default_conn) {
		default_conn = bt_conn_ref(conn);
	}

	gpio_pin_set_dt(&leds[1], 1);

	/* 接続後はアドバタイジングを停止 */
	adv_stop();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("[CONN] Disconnected: %s  reason=0x%02x", addr, reason);

	if (default_conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	target_found = false;
	gpio_pin_set_dt(&leds[1], 0);

	/* 切断後は自動的にアドバタイジングを再開 */
	adv_start();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = on_connected,
	.disconnected = on_disconnected,
};

/* ============================================================
 * ボタンコールバック
 *   全ボタンはPORT0上にあるため1つの関数で処理可能
 * ============================================================ */
static void on_button_pressed(const struct device *dev,
			      struct gpio_callback *cb,
			      uint32_t pins)
{
	if (pins & BIT(buttons[0].pin)) {
		/* BTN1: ADVトグル */
		LOG_INF(">>> BTN1: Toggle ADV");
		if (is_advertising) {
			adv_stop();
		} else {
			adv_start();
		}

	} else if (pins & BIT(buttons[1].pin)) {
		/* BTN2: Scanトグル */
		LOG_INF(">>> BTN2: Toggle Scan");
		if (is_scanning) {
			scan_stop();
		} else {
			scan_start();
		}

	} else if (pins & BIT(buttons[2].pin)) {
		/* BTN3: 接続 */
		LOG_INF(">>> BTN3: Connect");
		do_connect();

	} else if (pins & BIT(buttons[3].pin)) {
		/* BTN4: 切断 */
		LOG_INF(">>> BTN4: Disconnect");
		do_disconnect();
	}
}

/* ============================================================
 * 初期化
 * ============================================================ */

static int gpio_init(void)
{
	int err;

	/* LED初期化 */
	for (int i = 0; i < (int)ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			LOG_ERR("LED%d not ready", i);
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	/* ボタン初期化 (割り込み: 押した瞬間に発火) */
	for (int i = 0; i < (int)ARRAY_SIZE(buttons); i++) {
		if (!gpio_is_ready_dt(&buttons[i])) {
			LOG_ERR("BTN%d not ready", i);
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
		if (err) {
			return err;
		}
		err = gpio_pin_interrupt_configure_dt(&buttons[i],
						      GPIO_INT_EDGE_TO_ACTIVE);
		if (err) {
			return err;
		}
		gpio_init_callback(&btn_cb[i], on_button_pressed,
				   BIT(buttons[i].pin));
		gpio_add_callback(buttons[i].port, &btn_cb[i]);
	}

	return 0;
}

static void bt_ready_cb(int err)
{
	if (err) {
		LOG_ERR("BT init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	/* 起動時はアドバタイジング開始 (Androidから接続可能状態) */
	adv_start();
}

/* ============================================================
 * main
 * ============================================================ */
int main(void)
{
	int err;

	LOG_INF("======================================");
	LOG_INF("  nRF52840 BLE App");
	LOG_INF("  BTN1: ADV ON/OFF");
	LOG_INF("  BTN2: Scan ON/OFF");
	LOG_INF("  BTN3: Connect to found device");
	LOG_INF("  BTN4: Disconnect");
	LOG_INF("======================================");

	err = gpio_init();
	if (err) {
		LOG_ERR("GPIO init failed (err %d)", err);
		return err;
	}

	err = bt_enable(bt_ready_cb);
	if (err) {
		LOG_ERR("bt_enable failed (err %d)", err);
		return err;
	}

	/* メインループ: ボタン割り込みで制御するため待機のみ */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
