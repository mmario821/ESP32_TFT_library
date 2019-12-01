/* TFT demo

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tftspi.h"
#include "tft.h"
#include "esp_spiffs.h"

#include "driver/gpio.h"

#ifdef CONFIG_EXAMPLE_USE_WIFI

#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"

#endif


// ==========================================================
// Define which spi bus to use TFT_VSPI_HOST or TFT_HSPI_HOST
#define SPI_BUS TFT_HSPI_HOST
// ==========================================================

#define PIN_BUTTON_A 39
#define DISPLAY_ON_TIME_MSEC 5000

static struct tm* tm_info;
static char tmp_buff[64];
static time_t time_now, time_last = 0;

char imgbuf[40960];

#define SPIFFS_BASE_PATH ""

#define GDEMO_TIME 1000
#define GDEMO_INFO_TIME 5000

//==================================================================================
#ifdef CONFIG_EXAMPLE_USE_WIFI

static const char tag[] = "[TFT Demo]";

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = 0x00000001;

static xQueueHandle gpioEventQueue = NULL;

static void oneshot_timer_callback(void* arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    printf("One-shot timer called, time since boot: %lld us", time_since_boot);
	TFT_BacklightOff();
}

esp_timer_handle_t oneshot_timer;

const esp_timer_create_args_t oneshot_timer_args = {
		.callback = &oneshot_timer_callback,
		.name = "one-shot"
};


static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpioEventQueue, &gpio_num, NULL);
	// printf("!\n");
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpioEventQueue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
			TFT_BacklightOn();
			ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, DISPLAY_ON_TIME_MSEC*1000));
        }
    }
}


static void initialize_button_a()
{
	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_PIN_INTR_NEGEDGE;
	io_conf.pin_bit_mask = (1ULL<<PIN_BUTTON_A);
	io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);	

    //install gpio isr service
    gpio_install_isr_service(0);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(PIN_BUTTON_A, gpio_isr_handler, (void*) PIN_BUTTON_A);

    //create a queue to handle gpio event from isr
    gpioEventQueue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);
}

//------------------------------------------------------------
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
		printf("SYSTEM_EVENT_STA_START\n");
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
		printf("SYSTEM_EVENT_STA_GOT_IP\n");
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
		printf("SYSTEM_EVENT_STA_DISCONNECTED\n");
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

//-------------------------------
static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_LOGI(tag, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
	printf("Starting Wifi\n");
    ESP_ERROR_CHECK( esp_wifi_start() );
	printf("Wifi started\n");
}

//-------------------------------
static void initialize_sntp(void)
{
    ESP_LOGI(tag, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

//--------------------------
static int obtain_time(void)
{
	int res = 1;
    initialise_wifi();
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

    initialize_sntp();

    // wait for time to be set
    int retry = 0;
    const int retry_count = 20;

    time(&time_now);
	tm_info = localtime(&time_now);

    while(tm_info->tm_year < (2016 - 1900) && ++retry < retry_count) {
        //ESP_LOGI(tag, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
		sprintf(tmp_buff, "Wait %0d/%d", retry, retry_count);
    	TFT_print(tmp_buff, CENTER, LASTY);
		vTaskDelay(500 / portTICK_RATE_MS);
        time(&time_now);
    	tm_info = localtime(&time_now);
    }
    if (tm_info->tm_year < (2016 - 1900)) {
    	ESP_LOGI(tag, "System time NOT set.");
    	res = 0;
    }
    else {
    	ESP_LOGI(tag, "System time is set.");
    }

    ESP_ERROR_CHECK( esp_wifi_stop() );
    return res;
}

#endif  //CONFIG_EXAMPLE_USE_WIFI
//==================================================================================


//----------------------
static void _checkTime()
{
	time(&time_now);
	if (time_now > time_last) {
		color_t last_fg, last_bg;
		time_last = time_now;
		tm_info = localtime(&time_now);
		sprintf(tmp_buff, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

		TFT_saveClipWin();
		TFT_resetclipwin();

		Font curr_font = tft_cfont;
		last_bg = tft_bg;
		last_fg = tft_fg;
		tft_fg = TFT_YELLOW;
		tft_bg = (color_t){ 64, 64, 64 };
		TFT_setFont(DEFAULT_FONT, NULL);

		TFT_fillRect(1, tft_height-TFT_getfontheight()-8, tft_width-3, TFT_getfontheight()+6, tft_bg);
		TFT_print(tmp_buff, CENTER, tft_height-TFT_getfontheight()-5);

		tft_cfont = curr_font;
		tft_fg = last_fg;
		tft_bg = last_bg;

		TFT_restoreClipWin();
	}
}

//---------------------
static int Wait(int ms)
{
	uint8_t tm = 1;
	if (ms < 0) {
		tm = 0;
		ms *= -1;
	}
	if (ms <= 50) {
		vTaskDelay(ms / portTICK_RATE_MS);
		//if (_checkTouch()) return 0;
	}
	else {
		for (int n=0; n<ms; n += 50) {
			vTaskDelay(50 / portTICK_RATE_MS);
			if (tm) _checkTime();
			//if (_checkTouch()) return 0;
		}
	}
	return 1;
}

//---------------------
static void _dispTime()
{
	Font curr_font = tft_cfont;
    if (tft_width < 240) TFT_setFont(DEF_SMALL_FONT, NULL);
	else TFT_setFont(DEFAULT_FONT, NULL);

    time(&time_now);
	time_last = time_now;
	tm_info = localtime(&time_now);
	sprintf(tmp_buff, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
	TFT_print(tmp_buff, CENTER, tft_height-TFT_getfontheight()-5);

    tft_cfont = curr_font;
}

//---------------------------------
static void disp_header(char *info)
{
	TFT_fillScreen(TFT_BLACK);
	TFT_resetclipwin();

	tft_fg = TFT_YELLOW;
	tft_bg = (color_t){ 64, 64, 64 };

    if (tft_width < 240) TFT_setFont(DEF_SMALL_FONT, NULL);
	else TFT_setFont(DEFAULT_FONT, NULL);
	TFT_fillRect(0, 0, tft_width-1, TFT_getfontheight()+8, tft_bg);
	TFT_drawRect(0, 0, tft_width-1, TFT_getfontheight()+8, TFT_CYAN);

	TFT_fillRect(0, tft_height-TFT_getfontheight()-9, tft_width-1, TFT_getfontheight()+8, tft_bg);
	TFT_drawRect(0, tft_height-TFT_getfontheight()-9, tft_width-1, TFT_getfontheight()+8, TFT_CYAN);

	TFT_print(info, CENTER, 4);
	_dispTime();

	tft_bg = TFT_BLACK;
	TFT_setclipwin(0,TFT_getfontheight()+9, tft_width-1, tft_height-TFT_getfontheight()-10);
}

//---------------------------------------------
static void update_header(char *hdr, char *ftr)
{
	color_t last_fg, last_bg;

	TFT_saveClipWin();
	TFT_resetclipwin();

	Font curr_font = tft_cfont;
	last_bg = tft_bg;
	last_fg = tft_fg;
	tft_fg = TFT_YELLOW;
	tft_bg = (color_t){ 64, 64, 64 };
    if (tft_width < 240) TFT_setFont(DEF_SMALL_FONT, NULL);
	else TFT_setFont(DEFAULT_FONT, NULL);

	if (hdr) {
		TFT_fillRect(1, 1, tft_width-3, TFT_getfontheight()+6, tft_bg);
		TFT_print(hdr, CENTER, 4);
	}

	if (ftr) {
		TFT_fillRect(1, tft_height-TFT_getfontheight()-8, tft_width-3, TFT_getfontheight()+6, tft_bg);
		if (strlen(ftr) == 0) _dispTime();
		else TFT_print(ftr, CENTER, tft_height-TFT_getfontheight()-5);
	}

	tft_cfont = curr_font;
	tft_fg = last_fg;
	tft_bg = last_bg;

	TFT_restoreClipWin();
}

// Image demo
//-------------------------
static void disp_images() 
{
	// ** Show scaled (1/8, 1/4, 1/2 size) JPG images
	TFT_jpg_image(CENTER, CENTER, 0, NULL, (uint8_t*)imgbuf, sizeof(imgbuf));
	Wait(10000);
}

//---------------------
static void font_demo()
{
	// disp_header("7-SEG FONT DEMO");

	TFT_setFont(FONT_7SEG, NULL);
	int last_sec = 0;
	while (1) 
	{
		time(&time_now);
		tm_info = localtime(&time_now);
		if (tm_info->tm_sec != last_sec) {
			last_sec = tm_info->tm_sec;

			sprintf(tmp_buff, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
			set_7seg_font_atrib(12, 4, 0, TFT_DARKGREY);
			TFT_print(tmp_buff, CENTER, CENTER);
		}

		vTaskDelay(1000 / portTICK_RATE_MS);		
	}
}

//===============
void tft_demo() {

	tft_font_rotate = 0;
	tft_text_wrap = 0;
	tft_font_transparent = 0;
	tft_font_forceFixed = 0;
	TFT_resetclipwin();

	tft_image_debug = 0;

    char dtype[16];
    
    switch (tft_disp_type) {
        case DISP_TYPE_ILI9341:
            sprintf(dtype, "ILI9341");
            break;
        case DISP_TYPE_ILI9488:
            sprintf(dtype, "ILI9488");
            break;
        case DISP_TYPE_ST7789V:
            sprintf(dtype, "ST7789V");
            break;
        case DISP_TYPE_ST7735:
            sprintf(dtype, "ST7735");
            break;
        case DISP_TYPE_ST7735R:
            sprintf(dtype, "ST7735R");
            break;
        case DISP_TYPE_ST7735B:
            sprintf(dtype, "ST7735B");
            break;
        default:
            sprintf(dtype, "Unknown");
    }
    
	TFT_setRotation(LANDSCAPE);

	while (1) {
		font_demo();
	}
}

//=============
void app_main()
{
    // ========  PREPARE DISPLAY INITIALIZATION  =========

    esp_err_t ret;

    // === SET GLOBAL VARIABLES ==========================

	// ===================================================
	// ==== Set maximum spi clock for display read    ====
	//      operations, function 'find_rd_speed()'    ====
	//      can be used after display initialization  ====
	tft_max_rdclock = 8000000;
	// ===================================================

    // ====================================================================
    // === Pins MUST be initialized before SPI interface initialization ===
    // ====================================================================
    TFT_PinsInit();

    // ====  CONFIGURE SPI DEVICES(s)  ====================================================================================

    spi_lobo_device_handle_t spi;
	
    spi_lobo_bus_config_t buscfg={
        .miso_io_num=PIN_NUM_MISO,				// set SPI MISO pin
        .mosi_io_num=PIN_NUM_MOSI,				// set SPI MOSI pin
        .sclk_io_num=PIN_NUM_CLK,				// set SPI CLK pin
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
		.max_transfer_sz = 6*1024,
    };
    spi_lobo_device_interface_config_t devcfg={
        .clock_speed_hz=8000000,                // Initial clock out at 8 MHz
        .mode=0,                                // SPI mode 0
        .spics_io_num=-1,                       // we will use external CS pin
		.spics_ext_io_num=PIN_NUM_CS,           // external CS pin
		.flags=LB_SPI_DEVICE_HALFDUPLEX,        // ALWAYS SET  to HALF DUPLEX MODE!! for display spi
    };

    vTaskDelay(500 / portTICK_RATE_MS);
	printf("\r\n==============================\r\n");
    printf("TFT display DEMO, LoBo 11/2017\r\n");
	printf("==============================\r\n");
    printf("Pins used: miso=%d, mosi=%d, sck=%d, cs=%d\r\n", PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS);
	printf("==============================\r\n\r\n");

	// ==================================================================
	// ==== Initialize the SPI bus and attach the LCD to the SPI bus ====

	ret=spi_lobo_bus_add_device(SPI_BUS, &buscfg, &devcfg, &spi);
    assert(ret==ESP_OK);
	printf("SPI: display device added to spi bus (%d)\r\n", SPI_BUS);
	tft_disp_spi = spi;

	// ==== Test select/deselect ====
	ret = spi_lobo_device_select(spi, 1);
    assert(ret==ESP_OK);
	ret = spi_lobo_device_deselect(spi);
    assert(ret==ESP_OK);

	printf("SPI: attached display device, speed=%u\r\n", spi_lobo_get_speed(spi));
	printf("SPI: bus uses native pins: %s\r\n", spi_lobo_uses_native_pins(spi) ? "true" : "false");

	// ================================
	// ==== Initialize the Display ====

	printf("SPI: display init...\r\n");
	TFT_display_init();
    printf("OK\r\n");
	
	// ---- Detect maximum read speed ----
	tft_max_rdclock = find_rd_speed();
	printf("SPI: Max rd speed = %u\r\n", tft_max_rdclock);

    // ==== Set SPI clock used for display operations ====
	spi_lobo_set_speed(spi, DEFAULT_SPI_CLOCK);
	printf("SPI: Changed speed to %u\r\n", spi_lobo_get_speed(spi));

    printf("\r\n---------------------\r\n");
	printf("Graphics demo started\r\n");
	printf("---------------------\r\n");

	tft_font_rotate = 0;
	tft_text_wrap = 0;
	tft_font_transparent = 0;
	tft_font_forceFixed = 0;
    TFT_setGammaCurve(DEFAULT_GAMMA_CURVE);
	TFT_setRotation(LANDSCAPE);
	TFT_setFont(DEFAULT_FONT, NULL);
	TFT_resetclipwin();

#ifdef CONFIG_EXAMPLE_USE_WIFI

    ESP_ERROR_CHECK( nvs_flash_init() );

    // ===== Set time zone ======
	setenv("TZ", "CET-1CEST", 0);
	tzset();
	// ==========================

	disp_header("GET NTP TIME");

    time(&time_now);
	tm_info = localtime(&time_now);

	// Is time set? If not, tm_year will be (1970 - 1900).
    if (tm_info->tm_year < (2016 - 1900)) {
        ESP_LOGI(tag, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        tft_fg = TFT_CYAN;
    	TFT_print("Time is not set yet", CENTER, CENTER);
    	TFT_print("Connecting to WiFi", CENTER, LASTY+TFT_getfontheight()+2);
    	TFT_print("Getting time over NTP", CENTER, LASTY+TFT_getfontheight()+2);
    	tft_fg = TFT_YELLOW;
    	TFT_print("Wait", CENTER, LASTY+TFT_getfontheight()+2);
        if (obtain_time()) {
        	tft_fg = TFT_GREEN;
        	TFT_print("System time is set.", CENTER, LASTY);
        }
        else {
        	tft_fg = TFT_RED;
        	TFT_print("ERROR.", CENTER, LASTY);
        }
        time(&time_now);
    	update_header(NULL, "");
    	Wait(-2000);
    }
#endif

	disp_header("File system INIT");
    tft_fg = TFT_CYAN;
	TFT_print("Initializing SPIFFS...", CENTER, CENTER);

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t retFFS = esp_vfs_spiffs_register(&conf);

    if (retFFS != ESP_OK) {
        if (retFFS == ESP_FAIL) {
            ESP_LOGE(tag, "Failed to mount or format filesystem");
        } else if (retFFS == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(tag, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(tag, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
	else
	{
		ESP_LOGI(tag, "spiFFS inititalized");
	}
	
    size_t total = 0, used = 0;
    retFFS = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(tag, "Partition size: total: %d, used: %d", total, used);
    }

    // Open for reading hello.txt
    FILE* f = fopen("/spiffs/test1.jpg", "r");
    if (f == NULL) {
        ESP_LOGE(tag, "Failed to open test1.jpg");
        return;
    }
	else
	{
		ESP_LOGI(tag, "File opened with success");

		memset(imgbuf, 0, sizeof(imgbuf));
		int readBytes = fread(imgbuf, 1, sizeof(imgbuf), f);
		fclose(f);

		// Display the read contents from the file
		ESP_LOGI(tag, "Read from test1.jpg: %d", readBytes);
	}
	
	fclose(f);

	TFT_resetclipwin();
	TFT_invertDisplay(INVERT_ON);
	disp_images();

	initialize_button_a();

	ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));
	ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, DISPLAY_ON_TIME_MSEC*1000));

	//=========
    // Run demo
    //=========
	tft_demo();
}
