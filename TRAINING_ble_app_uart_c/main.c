/*
 * Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is confidential property of Nordic Semiconductor. The use,
 * copying, transfer or disclosure of such information is prohibited except by express written
 * agreement with Nordic Semiconductor.
 *
 */

/** @example Board/nrf6310/s120/experimental/ble_app_uart_c/main.c
 *
 * @brief BLE Heart Rate Collector application main file.
 *
 * This file contains the source code for a sample heart rate collector.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nordic_common.h"
#include "nrf_sdm.h"
#include "ble.h"
#include "ble_db_discovery.h"
#include "softdevice_handler.h"
#include "app_util.h"
#include "app_error.h"
#include "ble_advdata_parser.h"
#include "boards.h"
#include "nrf6350.h"
#include "nrf_gpio.h"
#include "pstorage.h"
#include "device_manager.h"
#include "app_trace.h"
#include "ble_uart_c.h"
#include "app_util.h"

#include "app_timer.h"
#define BOND_DELETE_ALL_BUTTON_ID  BUTTON_1                           /**< Button used for deleting all bonded centrals during startup. */
#define UART_SEND_BUTTON_PIN_NO			BUTTON_0
#define SCAN_LED_PIN_NO                  LED_0                                          /**< Is on when device is scanning. */
#define CONNECTED_LED_PIN_NO             LED_1                                          /**< Is on when device has connected. */
#define ASSERT_LED_PIN_NO                LED_7                                          /**< Is on when application has asserted. */

#define APPL_LOG                         app_trace_log                                  /**< Debug logger macro that will be used in this file to do logging of debug information over UART. */

#define SEC_PARAM_BOND             0                                  /**< Perform bonding. */
#define SEC_PARAM_MITM             1                                  /**< Man In The Middle protection not required. */
#define SEC_PARAM_IO_CAPABILITIES  BLE_GAP_IO_CAPS_NONE               /**< No I/O capabilities. */
#define SEC_PARAM_OOB              0                                  /**< Out Of Band data not available. */
#define SEC_PARAM_MIN_KEY_SIZE     7                                  /**< Minimum encryption key size. */
#define SEC_PARAM_MAX_KEY_SIZE     16                                 /**< Maximum encryption key size. */

#define SCAN_INTERVAL              0x00A0                             /**< Determines scan interval in units of 0.625 millisecond. */
#define SCAN_WINDOW                0x0050                             /**< Determines scan window in units of 0.625 millisecond. */

#define MIN_CONNECTION_INTERVAL    MSEC_TO_UNITS(7.5, UNIT_1_25_MS)   /**< Determines maximum connection interval in millisecond. */
#define MAX_CONNECTION_INTERVAL    MSEC_TO_UNITS(30, UNIT_1_25_MS)    /**< Determines maximum connection interval in millisecond. */
#define SLAVE_LATENCY              0                                  /**< Determines slave latency in counts of connection events. */
#define SUPERVISION_TIMEOUT        MSEC_TO_UNITS(4000, UNIT_10_MS)    /**< Determines supervision time-out in units of 10 millisecond. */

#define TARGET_UUID                0x180D                             /**< Target device name that application is looking for. */
#define MAX_PEER_COUNT             DEVICE_MANAGER_MAX_CONNECTIONS     /**< Maximum number of peer's application intends to manage. */
#define UUID16_SIZE                2                                  /**< Size of 16 bit UUID */
#define BUTTON_DETECTION_DELAY               APP_TIMER_TICKS(50, APP_TIMER_PRESCALER)   /**< Delay from a GPIOTE event until a button is reported as pushed (in number of timer ticks). */
#define APP_TIMER_PRESCALER                  0                                          /**< Value of the RTC1 PRESCALER register. */
#define APP_TIMER_MAX_TIMERS                 4                                          /**< Maximum number of simultaneously created timers. */
#define APP_TIMER_OP_QUEUE_SIZE              5                                          /**< Size of timer operation queues. */
#define UART_SEND_INTERVAL          APP_TIMER_TICKS(1000, APP_TIMER_PRESCALER) /**< Battery level measurement interval (ticks). */

/**@breif Macro to unpack 16bit unsigned UUID from octet stream. */
#define UUID16_EXTRACT(DST,SRC)                                                                  \
        do                                                                                       \
        {                                                                                        \
            (*(DST)) = (SRC)[1];                                                                 \
            (*(DST)) <<= 8;                                                                      \
            (*(DST)) |= (SRC)[0];                                                                \
        } while(0)

/**@brief Variable length data encapsulation in terms of length and pointer to data */
typedef struct
{
    uint8_t     * p_data;                                         /**< Pointer to data. */
    uint16_t      data_len;                                       /**< Length of data. */
}data_t;

typedef enum
{
    BLE_NO_SCAN,                                                  /**< No advertising running. */
    BLE_WHITELIST_SCAN,                                           /**< Advertising with whitelist. */
    BLE_FAST_SCAN,                                                /**< Fast advertising running. */
} ble_advertising_mode_t;

static ble_db_discovery_t           m_ble_db_discovery;                  /**< Structure used to identify the DB Discovery module. */
static ble_uart_c_t                  m_ble_uart_c;                         /**< Structure used to identify the heart rate client module. */

static ble_gap_scan_params_t        m_scan_param;                        /**< Scan parameters requested for scanning and connection. */
static dm_application_instance_t    m_dm_app_id;                         /**< Application identifier. */
static dm_handle_t                  m_dm_device_handle;                  /**< Device Identifier identifier. */
static uint8_t                      m_peer_count = 0;                    /**< Number of peer's connected. */
static uint8_t                      m_scan_mode;                         /**< Scan mode used by application. */

static bool                         m_memory_access_in_progress = false; /**< Flag to keep track of ongoing operations on persistent memory. */
static app_timer_id_t                        m_uart_send_timer_id;                        /**< Battery timer. */

uint8_t   nus_service_uuid[16] = {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                                     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E};
/**
 * @brief Connection parameters requested for connection.
 */
static const ble_gap_conn_params_t m_connection_param =
{
    (uint16_t)MIN_CONNECTION_INTERVAL,   // Minimum connection
    (uint16_t)MAX_CONNECTION_INTERVAL,   // Maximum connection
    0,                                   // Slave latency
    (uint16_t)SUPERVISION_TIMEOUT        // Supervision time-out
};

static void scan_start(void);

#define APPL_LOG                        app_trace_log             /**< Debug logger macro that will be used in this file to do logging of debug information over UART. */

// WARNING: The following macro MUST be un-defined (by commenting out the definition) if the user
// does not have a nRF6350 Display unit. If this is not done, the application will not work.
//#define APPL_LCD_PRINT_ENABLE                                     /**< In case you do not have a functional display unit, disable this flag and observe trace on UART. */

#ifdef APPL_LCD_PRINT_ENABLE

#define APPL_LCD_CLEAR                  nrf6350_lcd_clear         /**< Macro to clear the LCD display.*/
#define APPL_LCD_WRITE                  nrf6350_lcd_write_string  /**< Macro to write a string to the LCD display.*/

#else // APPL_LCD_PRINT_ENABLE

#define APPL_LCD_WRITE(...)             true                      /**< Macro to clear the LCD display defined to do nothing when @ref APPL_LCD_PRINT_ENABLE is not defined.*/
#define APPL_LCD_CLEAR(...)             true                      /**< Macro to write a string to the LCD display defined to do nothing when @ref APPL_LCD_PRINT_ENABLE is not defined.*/

#endif // APPL_LCD_PRINT_ENABLE


/**@brief Function for error handling, which is called when an error has occurred.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed to react in case of error.
 *
 * @param[in] error_code  Error code supplied to the handler.
 * @param[in] line_num    Line number where the handler is called.
 * @param[in] p_file_name Pointer to the file name.
 */
void app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name)
{
    APPL_LOG("[APPL]: ASSERT: %s, %d, error 0x%08x\r\n", p_file_name, line_num, error_code);
    nrf_gpio_pin_set(ASSERT_LED_PIN_NO);

    // This call can be used for debug purposes during development of an application.
    // @note CAUTION: Activating this code will write the stack to flash on an error.
    //                This function should NOT be used in a final product.
    //                It is intended STRICTLY for development/debugging purposes.
    //                The flash write will happen EVEN if the radio is active, thus interrupting
    //                any communication.
    //                Use with care. Un-comment the line below to use.
    // ble_debug_assert_handler(error_code, line_num, p_file_name);

    // On assert, the system can only recover with a reset.
		while(1);
    NVIC_SystemReset();
}


/**@brief Function for asserts in the SoftDevice.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed to react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in] line_num     Line number of the failing ASSERT call.
 * @param[in] p_file_name  File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(0xDEADBEEF, line_num, p_file_name);
}


/**@brief Callback handling device manager events.
 *
 * @details This function is called to notify the application of device manager events.
 *
 * @param[in]   p_handle      Device Manager Handle. For link related events, this parameter
 *                            identifies the peer.
 * @param[in]   p_event       Pointer to the device manager event.
 * @param[in]   event_status  Status of the event.
 */
static api_result_t device_manager_event_handler(const dm_handle_t    * p_handle,
                                                 const dm_event_t     * p_event,
                                                 const api_result_t     event_result)
{
    bool     lcd_write_status;
    uint32_t err_code;

    switch(p_event->event_id)
    {
        case DM_EVT_CONNECTION:
        {
            APPL_LOG("[APPL]: >> DM_EVT_CONNECTION\r\n");
#ifdef ENABLE_DEBUG_LOG_SUPPORT
            ble_gap_addr_t * peer_addr;
            peer_addr = &p_event->event_param.p_gap_param->params.connected.peer_addr;
#endif // ENABLE_DEBUG_LOG_SUPPORT
            APPL_LOG("[APPL]:[%02X %02X %02X %02X %02X %02X]: Connection Established\r\n",
                                peer_addr->addr[0], peer_addr->addr[1], peer_addr->addr[2],
                                peer_addr->addr[3], peer_addr->addr[4], peer_addr->addr[5]);
            
            nrf_gpio_pin_set(CONNECTED_LED_PIN_NO);
            lcd_write_status = APPL_LCD_WRITE("Connected", 9, LCD_UPPER_LINE, 0);
            if (!lcd_write_status)
            {
                APPL_LOG("[APPL]: LCD Write failed!\r\n");
            }
            m_dm_device_handle = (*p_handle);

            // Discover peer's services. 
             err_code = ble_db_discovery_start(&m_ble_db_discovery,
                                               p_event->event_param.p_gap_param->conn_handle);
            APP_ERROR_CHECK(err_code);

            m_peer_count++;
            if (m_peer_count < MAX_PEER_COUNT)
            {
                scan_start();
            }
            APPL_LOG("[APPL]: << DM_EVT_CONNECTION\r\n");
            break;
        }
        
        case DM_EVT_DISCONNECTION:
        {
            APPL_LOG("[APPL]: >> DM_EVT_DISCONNECTION\r\n");
            memset(&m_ble_db_discovery, 0 , sizeof (m_ble_db_discovery));
            lcd_write_status = APPL_LCD_CLEAR();
            if (!lcd_write_status)
            {
                APPL_LOG("[APPL]: LCD Clear failed!\r\n");
            }

            lcd_write_status = APPL_LCD_WRITE("Disconnected", 12, LCD_UPPER_LINE, 0);
            if (!lcd_write_status)
            {
                APPL_LOG("[APPL]:[4]: LCD Write failed!\r\n");
            }

            nrf_gpio_pin_clear(CONNECTED_LED_PIN_NO);
            if (m_peer_count == MAX_PEER_COUNT)
            {
                scan_start();
            }
            m_peer_count--;
            APPL_LOG("[APPL]: << DM_EVT_DISCONNECTION\r\n");
            break;
        }
        
        case DM_EVT_SECURITY_SETUP:
        {
            APPL_LOG("[APPL]:[0x%02X] >> DM_EVT_SECURITY_SETUP\r\n", p_handle->connection_id);
            // Slave securtiy request received from peer, if from a non bonded device, 
            // initiate security setup, else, wait for encryption to complete.
            err_code = dm_security_setup_req(&m_dm_device_handle);
            APP_ERROR_CHECK(err_code);
            APPL_LOG("[APPL]:[0x%02X] << DM_EVT_SECURITY_SETUP\r\n", p_handle->connection_id);
            break;
        }
        case DM_EVT_SECURITY_SETUP_COMPLETE:
        {
            APPL_LOG("[APPL]: >> DM_EVT_SECURITY_SETUP_COMPLETE\r\n");            
             // Heart rate service discovered. Enable notification of Heart Rate Measurement.
            err_code = ble_uart_c_hrm_notif_enable(&m_ble_uart_c);
            APP_ERROR_CHECK(err_code);
            APPL_LOG("[APPL]: << DM_EVT_SECURITY_SETUP_COMPLETE\r\n");
            break;
        }
        
        case DM_EVT_LINK_SECURED:
            APPL_LOG("[APPL]: >> DM_LINK_SECURED_IND\r\n");
            APPL_LOG("[APPL]: << DM_LINK_SECURED_IND\r\n");
            break;
            
        case DM_EVT_DEVICE_CONTEXT_LOADED:
            APPL_LOG("[APPL]: >> DM_EVT_LINK_SECURED\r\n");
            APP_ERROR_CHECK(event_result);
            APPL_LOG("[APPL]: << DM_EVT_DEVICE_CONTEXT_LOADED\r\n");
            break;
            
        case DM_EVT_DEVICE_CONTEXT_STORED:
            APPL_LOG("[APPL]: >> DM_EVT_DEVICE_CONTEXT_STORED\r\n");
            APP_ERROR_CHECK(event_result);
            APPL_LOG("[APPL]: << DM_EVT_DEVICE_CONTEXT_STORED\r\n");
            break;
            
        case DM_EVT_DEVICE_CONTEXT_DELETED:
            APPL_LOG("[APPL]: >> DM_EVT_DEVICE_CONTEXT_DELETED\r\n");
            APP_ERROR_CHECK(event_result);
            APPL_LOG("[APPL]: << DM_EVT_DEVICE_CONTEXT_DELETED\r\n");
            break;
            
        default:
            break;
    }

    return NRF_SUCCESS;
}


/**
 * @brief Parses advertisement data, providing length and location of the field in case
 *        matching data is found.
 *
 * @param[in]  Type of data to be looked for in advertisement data.
 * @param[in]  Advertisement report length and pointer to report.
 * @param[out] If data type requested is found in the data report, type data length and
 *             pointer to data will be populated here.
 *
 * @retval NRF_SUCCESS if the data type is found in the report.
 * @retval NRF_ERROR_NOT_FOUND if the data type could not be found.
 */
static uint32_t adv_report_parse(uint8_t type, data_t * p_advdata, data_t * p_typedata)
{
    uint32_t index = 0;
    uint8_t * p_data;

    p_data = p_advdata->p_data;

    while (index < p_advdata->data_len)
    {
        uint8_t field_length = p_data[index];
        uint8_t field_type = p_data[index+1];

        if (field_type == type)
        {
            p_typedata->p_data = &p_data[index+2];
            p_typedata->data_len = field_length-1;
            return NRF_SUCCESS;
        }
        index += field_length+1;
    }
    return NRF_ERROR_NOT_FOUND;
}



/**@brief Function for handling the Application's BLE Stack events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 */
static void on_ble_evt(ble_evt_t * p_ble_evt)
{
    uint32_t                err_code;
    const ble_gap_evt_t   * p_gap_evt = &p_ble_evt->evt.gap_evt;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_ADV_REPORT:
        {
            data_t adv_data;
            data_t type_data;
						APPL_LOG("\t[APPL]: Catched an advertising packet, check for UUID match\r\n");
            // Initialize advertisement report for parsing.
            adv_data.p_data = (uint8_t *)p_gap_evt->params.adv_report.data;
            adv_data.data_len = p_gap_evt->params.adv_report.dlen;

            err_code = adv_report_parse(BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_MORE_AVAILABLE,
                                        &adv_data,
                                        &type_data);
            if (err_code != NRF_SUCCESS)
            {
                // Compare 128 UUID.
                err_code = adv_report_parse(BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE,
                                            &adv_data,
                                            &type_data);
            }
						//APPL_LOG("\t[APPL]: %x%x\r\n",type_data.p_data[0],type_data.p_data[1]);
            // Verify if short or complete name matches target.
            if (err_code == NRF_SUCCESS)
            {
               
											
                    if(!memcmp( nus_service_uuid,type_data.p_data,16))
                    {
											APPL_LOG("\t[APPL]: UUID matched\r\n");
                        // Stop scanning.
                        err_code = sd_ble_gap_scan_stop();
                        if (err_code != NRF_SUCCESS)
                        {
                            APPL_LOG("[APPL]: Scan stop failed, reason %d\r\n", err_code);
                        }
                        nrf_gpio_pin_clear(SCAN_LED_PIN_NO);
                        
                        m_scan_param.selective = 0; 

                        // Initiate connection.
                        err_code = sd_ble_gap_connect(&p_gap_evt->params.adv_report.\
                                                       peer_addr,
                                                       &m_scan_param,
                                                       &m_connection_param);

                        if (err_code != NRF_SUCCESS)
                        {
                            APPL_LOG("[APPL]: Connection Request Failed, reason %d\r\n", err_code);
                        }
                        break;
                    }
               // }
            }
            break;
        }
        case BLE_GAP_EVT_TIMEOUT:
            if(p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_SCAN)
            {
                APPL_LOG("[APPL]: Scan timed out.\r\n");
                if (m_scan_mode ==  BLE_WHITELIST_SCAN)
                {
                    m_scan_mode = BLE_FAST_SCAN;

                    // Start non selective scanning.
                    scan_start();
                }
            }
            else if (p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN)
            {
                APPL_LOG("[APPL]: Connection Request timed out.\r\n");
            }
            break;
        case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
            // Accepting parameters requested by peer.
            err_code = sd_ble_gap_conn_param_update(p_gap_evt->conn_handle,
                                                    &p_gap_evt->params.conn_param_update_request.conn_params);
            APP_ERROR_CHECK(err_code);
            break;
        default:
            break;
    }
}

/**@brief Function for handling the Application's system events.
 *
 * @param[in]   sys_evt   system event.
 */
static void on_sys_evt(uint32_t sys_evt)
{
    switch(sys_evt)
    {
        case NRF_EVT_FLASH_OPERATION_SUCCESS:
        case NRF_EVT_FLASH_OPERATION_ERROR:
            if (m_memory_access_in_progress)
            {
                m_memory_access_in_progress = false;
                scan_start();
            }
            break;
        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for dispatching a BLE stack event to all modules with a BLE stack event handler.
 *
 * @details This function is called from the scheduler in the main loop after a BLE stack event has
 *  been received.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 */
static void ble_evt_dispatch(ble_evt_t * p_ble_evt)
{
    dm_ble_evt_handler(p_ble_evt);
    ble_db_discovery_on_ble_evt(&m_ble_db_discovery, p_ble_evt);
    ble_uart_c_on_ble_evt(&m_ble_uart_c, p_ble_evt);
//    ble_bas_c_on_ble_evt(&m_ble_bas_c, p_ble_evt);
    on_ble_evt(p_ble_evt);
}


/**@brief Function for dispatching a system event to interested modules.
 *
 * @details This function is called from the System event interrupt handler after a system
 *          event has been received.
 *
 * @param[in]   sys_evt   System stack event.
 */
static void sys_evt_dispatch(uint32_t sys_evt)
{
    pstorage_sys_event_handler(sys_evt);
    on_sys_evt(sys_evt);
}


/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    uint32_t err_code;

    // Initialize the SoftDevice handler module.
    SOFTDEVICE_HANDLER_INIT(NRF_CLOCK_LFCLKSRC_XTAL_20_PPM, false);

    // Register with the SoftDevice handler module for BLE events.
    err_code = softdevice_ble_evt_handler_set(ble_evt_dispatch);
    APP_ERROR_CHECK(err_code);

    // Register with the SoftDevice handler module for System events.
    err_code = softdevice_sys_evt_handler_set(sys_evt_dispatch);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the Device Manager.
 *
 * @details Device manager is initialized here.
 */
static void device_manager_init(void)
{
    dm_application_param_t param;
    dm_init_param_t        init_param;

    uint32_t              err_code;

    err_code = pstorage_init();
    APP_ERROR_CHECK(err_code);

    // Clear all bonded devices if user requests to.
    init_param.clear_persistent_data =
        ((nrf_gpio_pin_read(BOND_DELETE_ALL_BUTTON_ID) == 0)? true: false);

    err_code = dm_init(&init_param);
    APP_ERROR_CHECK(err_code);

    memset(&param.sec_param, 0, sizeof (ble_gap_sec_params_t));

    // Event handler to be registered with the module.
    param.evt_handler            = device_manager_event_handler;

    // Service or protocol context for device manager to load, store and apply on behalf of application.
    // Here set to client as application is a GATT client.
    param.service_type           = DM_PROTOCOL_CNTXT_GATT_CLI_ID;

    // Secuirty parameters to be used for security procedures.
    param.sec_param.bond         = SEC_PARAM_BOND;
    param.sec_param.mitm         = SEC_PARAM_MITM;
    param.sec_param.io_caps      = SEC_PARAM_IO_CAPABILITIES;
    param.sec_param.oob          = SEC_PARAM_OOB;
    param.sec_param.min_key_size = SEC_PARAM_MIN_KEY_SIZE;
    param.sec_param.max_key_size = SEC_PARAM_MAX_KEY_SIZE;
    param.sec_param.kdist_periph.enc = 1;
    param.sec_param.kdist_periph.id  = 1;

    err_code = dm_register(&m_dm_app_id,&param);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for the LEDs initialization.
 *
 * @details Initializes all LEDs used by this application.
 */
static void leds_init(void)
{
    nrf_gpio_cfg_output(SCAN_LED_PIN_NO);
    nrf_gpio_cfg_output(CONNECTED_LED_PIN_NO);
    nrf_gpio_cfg_output(ASSERT_LED_PIN_NO);
}





/** @brief Function for the initialization of the nRF 6350 display.
 */
void nrf6350_init(void)
{
#ifdef APPL_LCD_PRINT_ENABLE
    bool success;

    success = nrf6350_lcd_init();
    if (success)
    {
        success = nrf6350_lcd_on();
        APP_ERROR_CHECK_BOOL(success);

        success = nrf6350_lcd_set_contrast(LCD_CONTRAST_HIGH);
        APP_ERROR_CHECK_BOOL(success);
    }
#endif // APPL_LCD_PRINT_ENABLE
}


/** @brief Function for the Power manager.
 */
static void power_manage(void)
{
    uint32_t err_code = sd_app_evt_wait();
    APP_ERROR_CHECK(err_code);
}


/**@brief Heart Rate Collector Handler.
 */
static void uart_c_evt_handler(ble_uart_c_t * p_uart_c, ble_uart_c_evt_t * p_uart_c_evt)
{
    bool     success;
    uint32_t err_code;

    switch (p_uart_c_evt->evt_type)
    {
        case BLE_uart_C_EVT_DISCOVERY_COMPLETE:
            // Initiate bonding.
            err_code = dm_security_setup_req(&m_dm_device_handle);
            APP_ERROR_CHECK(err_code);
            
            // Heart rate service discovered. Enable notification of Heart Rate Measurement.
            err_code = ble_uart_c_hrm_notif_enable(p_uart_c);
            APP_ERROR_CHECK(err_code);
						 write_dummy();
						err_code = app_timer_start(m_uart_send_timer_id, UART_SEND_INTERVAL, NULL);
						APP_ERROR_CHECK(err_code);
            //success = APPL_LCD_WRITE("Heart Rate", 10, LCD_UPPER_LINE, 0);
            //APP_ERROR_CHECK_BOOL(success);
            break;

        case BLE_UART_C_EVT_HRM_NOTIFICATION:
        {
						p_uart_c_evt->params.uart.rx_data[p_uart_c_evt->params.uart.len]=0;
						APPL_LOG("[APPL]: TX received: %s", p_uart_c_evt->params.uart.rx_data);
						

            char hr_as_string[LCD_LLEN];

           
            break;
        }
        default:
            break;
    }
}





/**
 * @brief UART service initialization.
 */
static void uart_c_init(void)
{
    ble_uart_c_init_t uart_c_init_obj;

    uart_c_init_obj.evt_handler = uart_c_evt_handler;

    uint32_t err_code = ble_uart_c_init(&m_ble_uart_c, &uart_c_init_obj);
    APP_ERROR_CHECK(err_code);
}





/**
 * @brief Database discovery collector initialization.
 */
static void db_discovery_init(void)
{
    uint32_t err_code = ble_db_discovery_init();
    APP_ERROR_CHECK(err_code);
}


/**@breif Function to start scanning.
 */
static void scan_start(void)
{
    ble_gap_whitelist_t   whitelist;
    ble_gap_addr_t        * p_whitelist_addr[BLE_GAP_WHITELIST_ADDR_MAX_COUNT];
    ble_gap_irk_t         * p_whitelist_irk[BLE_GAP_WHITELIST_IRK_MAX_COUNT];
    uint32_t              err_code;
    uint32_t              count;

    // Verify if there is any flash access pending, if yes delay starting scanning until 
    // it's complete.
    err_code = pstorage_access_status_get(&count);
    APP_ERROR_CHECK(err_code);
    
    if (count != 0)
    {
        m_memory_access_in_progress = true;
        return;
    }
    
    // Initialize whitelist parameters.
    whitelist.addr_count = BLE_GAP_WHITELIST_ADDR_MAX_COUNT;
    whitelist.irk_count  = 0;
    whitelist.pp_addrs   = p_whitelist_addr;
    whitelist.pp_irks    = p_whitelist_irk;

    // Request creating of whitelist.
    err_code = dm_whitelist_create(&m_dm_app_id,&whitelist);
    APP_ERROR_CHECK(err_code);

    if (((whitelist.addr_count == 0) && (whitelist.irk_count == 0)) ||
         (m_scan_mode != BLE_WHITELIST_SCAN))
    {
        // No devices in whitelist, hence non selective performed.
        m_scan_param.active       = 1;            // Active scanning set.
        m_scan_param.selective    = 0;            // Selective scanning not set.
        m_scan_param.interval     = SCAN_INTERVAL;// Scan interval.
        m_scan_param.window       = SCAN_WINDOW;  // Scan window.
        m_scan_param.p_whitelist  = NULL;         // No whitelist provided.
        m_scan_param.timeout      = 0x0000;       // No timeout.
    }
    else
    {
        // Selective scanning based on whitelist first.
        m_scan_param.active       = 1;            // Active scanning set.
        m_scan_param.selective    = 1;            // Selective scanning not set.
        m_scan_param.interval     = SCAN_INTERVAL;// Scan interval.
        m_scan_param.window       = SCAN_WINDOW;  // Scan window.
        m_scan_param.p_whitelist  = &whitelist;   // Provide whitelist.
        m_scan_param.timeout      = 0x001E;       // 30 seconds timeout.

        // Set whitelist scanning state.
        m_scan_mode = BLE_WHITELIST_SCAN;
    }

    err_code = sd_ble_gap_scan_start(&m_scan_param);
    APP_ERROR_CHECK(err_code);
    
    bool lcd_write_status = APPL_LCD_WRITE("Scanning", 8, LCD_UPPER_LINE, 0);
    if (!lcd_write_status)
    {
        APPL_LOG("[APPL]: LCD Write failed!\r\n");
    }

    nrf_gpio_pin_set(SCAN_LED_PIN_NO);
}

//** Timer handler to send dummy data to peer every one second

static void uart_send_timeout_handler(void * p_context)
{
	write_dummy();
}

static void timers_init(void)
{
    uint32_t err_code;

    // Initialize timer module.
    APP_TIMER_INIT(APP_TIMER_PRESCALER, 1, 2, false);
		err_code = app_timer_create(&m_uart_send_timer_id,
                                APP_TIMER_MODE_REPEATED,
                               uart_send_timeout_handler);
    APP_ERROR_CHECK(err_code);
    // Create timers.
   
}


int main(void)
{
    // Initialization of various modules.
    app_trace_init();
		APPL_LOG("[APPL]: Start...\r\n");
    leds_init();
		timers_init();
   
    nrf6350_init();
    ble_stack_init();
    device_manager_init();
    db_discovery_init();
    uart_c_init();
	
    // Start scanning for peripherals and initiate connection
    // with devices that advertise Heart Rate UUID.
    scan_start();

    for (;;)
    {
        power_manage();
    }
}


