#include "codex_net.h"

#include <stdio.h>
#include <string.h>

#include "codex_defaults.h"
#include "codex_state.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "codex_net";

/*
 * 板载以太网引脚（JC1060P470C_I_W_Y）
 *
 * 取自卖家 ethernet demo 的 sdkconfig 实测值，不是上游 esp-bsp 的通用默认值。
 * 上游 sdkconfig.ci.default_ip101 里写的 MDC=23 / MDIO=18 / RST=5 是普通 ESP32
 * 开发板的引脚，照抄到 P4 这块板子上 PHY 不响应。
 */
#define CODEX_ETH_MDC_GPIO 31
#define CODEX_ETH_MDIO_GPIO 52
#define CODEX_ETH_PHY_RST_GPIO 51
#define CODEX_ETH_PHY_ADDR 1

/**
 * 以太网路由优先级
 *
 * 必须高于 WIFI_STA 的默认值 100：两条链路同时有 IP 时，esp_netif 按 route_prio
 * 选默认网关，用默认值会让 WiFi 抢走出口流量，网线反而成了摆设。
 */
#define CODEX_ETH_ROUTE_PRIO 128

/** WiFi 断开后的重连间隔：不停重试会把串口刷满，也没意义 */
#define CODEX_WIFI_RETRY_MS 5000

#define NET_UP_BIT BIT0

static EventGroupHandle_t s_net_events;
static esp_timer_handle_t s_wifi_retry;

/* 两条链路各记自己的 IP，生效链路由 publish_link 统一裁决 */
static char s_eth_ip[16];
static char s_wifi_ip[16];

/**
 * 按「有线优先」裁决当前生效链路并发布
 *
 * 只有这一个地方写 codex_state 的链路，两个事件源各自只更新自己那份 IP，
 * 免得网线插拔和 WiFi 重连交错时把状态改成互相矛盾的组合。
 */
static void publish_link(void)
{
    if (s_eth_ip[0]) {
        codex_state_set_net(CODEX_LINK_ETH, s_eth_ip);
    } else if (s_wifi_ip[0]) {
        codex_state_set_net(CODEX_LINK_WIFI, s_wifi_ip);
    } else {
        codex_state_set_net(CODEX_LINK_NONE, "");
    }

    if (s_eth_ip[0] || s_wifi_ip[0]) {
        xEventGroupSetBits(s_net_events, NET_UP_BIT);
    } else {
        xEventGroupClearBits(s_net_events, NET_UP_BIT);
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "网线已连接，等待 DHCP");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "网线已断开%s", s_wifi_ip[0] ? "，切到 WiFi" : "");
        s_eth_ip[0] = '\0';
        publish_link();
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    char *slot = (id == IP_EVENT_ETH_GOT_IP) ? s_eth_ip : s_wifi_ip;
    snprintf(slot, sizeof(s_eth_ip), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "%s 已获取 IP %s", id == IP_EVENT_ETH_GOT_IP ? "有线" : "WiFi", slot);

    /*
     * 无线链路质量。这块屏装在带金属背板的面板里，C6 的天线八成被屏和背板挡着，
     * 实测 ping 面板平均 1000ms、丢包 13~25%，而 ping 路由器 1ms、0% —— 抖的全在
     * 无线这一段。RSSI 是区分「信号弱」和「软件问题」的唯一硬数据，拿到 IP 这一刻
     * 打一条就够，别周期刷。取不到（RPC 失败）就只打 IP，不影响正常路径。
     */
    if (id != IP_EVENT_ETH_GOT_IP) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi 信号 rssi=%d dBm，信道 %d，%s",
                     ap.rssi, ap.primary, ap.ssid);
        }
    }

    publish_link();
}

static void wifi_retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi 已关联，等待 DHCP");
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        /* 只在真正掉过 IP 时才吭声，纯关联失败每 5 秒一次会把日志刷满 */
        if (s_wifi_ip[0]) {
            ESP_LOGW(TAG, "WiFi 已断开（reason=%d）", e->reason);
            s_wifi_ip[0] = '\0';
            publish_link();
        }
        esp_timer_start_once(s_wifi_retry, (uint64_t)CODEX_WIFI_RETRY_MS * 1000);
        break;
    }
    default:
        break;
    }
}

/**
 * 启动 WiFi STA（经板载 ESP32-C6，esp-hosted over SDIO）
 *
 * 只在编译期配了 SSID 时调用：WiFi 协议栈那份 RAM 和 C6 的复位/握手都是白花的
 * 开销，没配无线时还会被连接失败的日志刷屏。
 */
static esp_err_t wifi_start(void)
{
    /*
     * STA 的 route_prio 保持默认，靠把以太网抬到 128 来实现有线优先 —— 改自己
     * 创建的 netif 比改 esp_netif_create_default_wifi_sta() 内部的配置干净。
     */
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "创建 WiFi STA netif 失败");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        /* 十有八九是 C6 那边没应答：slave 固件没烧、版本不匹配，或 SDIO 接线异常 */
        ESP_LOGE(TAG, "WiFi 初始化失败（%s），检查 C6 协处理器固件", esp_err_to_name(err));
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_wifi_retry));

    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, CODEX_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CODEX_WIFI_PASS, sizeof(wifi_cfg.sta.password));
    /*
     * 不设 threshold.authmode：默认门槛最低，什么加密的 AP 都能连上。设成
     * WPA2_PSK 会让 WPA/TKIP 的老 AP 直接连不上，而界面上只显示「未连接」，
     * 看不出是被自己的门槛挡了。
     */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * 关掉 modem sleep。默认的省电会让射频周期性休眠，实测这条链路 ping 面板
     * 平均 716ms、丢包 30%，而 ping 路由器 1ms、0% —— 抖的全是无线这一段。表现
     * 在界面上就是拖音量条手指早松了数字才动，串口里还时不时冒一条
     * "Connection timed out before data was ready"。
     *
     * 这块屏是常供电的看板，不为电池考虑，省电带来的那点电流毫无价值。
     * esp-hosted 下这个调用会 RPC 到 C6，万一 slave 固件不认就只告警：延迟退回
     * 原样而已，不值得让整个面板重启。
     */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "关闭 WiFi 省电失败（%s），链路延迟会偏高", esp_err_to_name(ps_err));
    }

    ESP_LOGI(TAG, "WiFi 已启动，SSID %s（兜底链路，有线优先）", CODEX_WIFI_SSID);
    return ESP_OK;
}

static esp_err_t eth_start(void)
{
    esp_netif_inherent_config_t eth_base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    eth_base.route_prio = CODEX_ETH_ROUTE_PRIO;
    esp_netif_config_t netif_cfg = {
        .base = &eth_base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    if (!netif) {
        ESP_LOGE(TAG, "创建以太网 netif 失败");
        return ESP_FAIL;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CODEX_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CODEX_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CODEX_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CODEX_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "EMAC / PHY 实例创建失败");
        return ESP_FAIL;
    }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "以太网已启动（IP101 @ MDC=%d MDIO=%d RST=%d ADDR=%d）", CODEX_ETH_MDC_GPIO,
             CODEX_ETH_MDIO_GPIO, CODEX_ETH_PHY_RST_GPIO, CODEX_ETH_PHY_ADDR);
    return ESP_OK;
}

esp_err_t codex_net_start(void)
{
    s_net_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t err = eth_start();
    if (err != ESP_OK) return err;

    if (CODEX_WIFI_SSID[0]) {
        /* WiFi 起不来不算致命：有线还在 */
        if (wifi_start() != ESP_OK) {
            ESP_LOGW(TAG, "WiFi 不可用，仅使用有线");
        }
    } else {
        ESP_LOGI(TAG, "未配置 WiFi，仅使用有线");
    }
    return ESP_OK;
}

bool codex_net_is_up(void)
{
    if (!s_net_events) return false;
    return (xEventGroupGetBits(s_net_events) & NET_UP_BIT) != 0;
}

bool codex_net_wait_up(int timeout_ms)
{
    if (!s_net_events) return false;
    EventBits_t bits =
        xEventGroupWaitBits(s_net_events, NET_UP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & NET_UP_BIT) != 0;
}
