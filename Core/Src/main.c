/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"
#include "app_x-cube-ai.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "app_x-cube-ai.h"
#include "ai_datatypes_defines.h"
#include "network.h"
#include "network_data.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* D1 (the PC7 status LED) is wired active-low: confirmed by testing that
   the pin explicitly held at RESET throughout STOP sleep still showed
   solid-on. Route every LED write through these two so polarity lives in
   exactly one place. */
#define LED_ON()   HAL_GPIO_WritePin(LED_control_GPIO_Port, LED_control_Pin, GPIO_PIN_RESET)
#define LED_OFF()  HAL_GPIO_WritePin(LED_control_GPIO_Port, LED_control_Pin, GPIO_PIN_SET)

#define ICM_ASSERT_ENABLE

#ifdef ICM_ASSERT_ENABLE
	//#define ICM_ASSERT(cr) if(cr){printf("Icm assert!!, file: %s, line: %d", __FILE__, __LINE__);while(1){};}
	#define ICM_ASSERT(cr) if(cr){ HAL_UART_Transmit(&huart2, (uint8_t*)"Icm Assert!\r\n", 13, 100); LED_ON();}
#else
	#define ICM_ASSERT(cr)
#endif
		
#define ICM20948_ADDR     0x68
#define ICM_I2C_READ(addr, pData, len) HAL_I2C_Mem_Read(&hi2c1, ICM20948_ADDR<<1 | 0x1, addr, 1, pData, len, 1000);
#define ICM_I2C_WRITE(addr, pData, len) HAL_I2C_Mem_Write(&hi2c1, ICM20948_ADDR<<1 | 0x0, addr, 1, pData, len, 1000);
#define WINDOW_FRAMES 297
#define NUM_AXES 6

/* Power control: PC6 is a plain push button (no hardware power latch), and
   the MCU/BT 3V3 rail cannot be switched off in hardware - only the 1V8
   IMU rail (PA11) can. "Power off" is therefore implemented as: cut the
   IMU rail and put the MCU into STOP mode, woken back up by a PC6 press. */
#define PWR_LONG_PRESS_MS   1500u
#define PWR_LED_BLINK_MS    120u

/* Fallback BLE-link indicator on PC7 (D5, wired straight to the HM19
   module's own STATE pin, is not connected to the MCU at all - so it
   cannot be driven or read from firmware). The HM19 module itself prints
   unsolicited status text on the same UART line: "OK+CONN" when a phone
   connects, "OK+LOST" when it disconnects. That is watched for directly
   (no timeout/heuristic needed - these are real connect/disconnect
   events). Not connected -> fast blink. Connected -> slow blink. */
#define BT_BLINK_FAST_MS      150u
#define BT_BLINK_SLOW_MS      800u
#define BT_NOTIFY_CONNECTED   "OK+CONN"
#define BT_NOTIFY_LOST        "OK+LOST"
#define BT_NOTIFY_BUF_LEN     16u
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
typedef struct{
	uint8_t currRegBank;
} IcmCb_t;


typedef struct
{
	float x;
	float y;
	float z;
	uint16_t ux;
	uint16_t uy;
	uint16_t uz;
} axises3;


axises3 my_gyro;
axises3 my_accel;

const float butterworth_biquad_coeffs[10] = {
    0.0007509f, 0.0015019f, 0.0007509f, 1.3982779f, -0.4995384f,
    1.0000000f, 2.0000000f, 1.0000000f, 1.6384440f, -0.7570968f,
};

IcmCb_t g_icmCb;
void GetIcmAccelRawData(IcmCb_t *pIcmCb, axises3* data);
void GetIcmGyroRawData(IcmCb_t *pIcmCb, axises3* data);
bool IsIcmRawDataRdy(IcmCb_t *pIcmCb);
bool IcmInit(IcmCb_t *pIcmCb);
void IcmConfig(IcmCb_t *pIcmCb);
uint8_t IcmWrite(IcmCb_t *pIcmCb ,uint8_t bank, uint8_t regAddr, uint8_t *pRegData, uint8_t length);
void HM19_SendATCommand(const char* cmd, char* response, uint16_t respLen);
void HM19_ConfigureAsSlaveWithName(void);
void Apply_ETL();

static void Led_Blink(uint8_t times, uint32_t period_ms);
static bool PowerButton_HeldFor(uint32_t ms);
static void PowerManager_SleepUntilLongPress(void);
static bool System_PowerOn(void);
static void System_PowerOff(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
float raw_data_buffer[WINDOW_FRAMES][NUM_AXES];
float processed_buffer[WINDOW_FRAMES][NUM_AXES];

ai_handle network;
ai_u8 activations[AI_NETWORK_DATA_ACTIVATIONS_SIZE];
ai_buffer *ai_input;
ai_buffer *ai_output;
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART2_UART_Init();
  //MX_X_CUBE_AI_Init();
  /* USER CODE BEGIN 2 */
	HAL_GPIO_WritePin(Pin3v3_GPIO_Port, Pin3v3_Pin, GPIO_PIN_SET);
	HM19_ConfigureAsSlaveWithName();

	ai_error err = ai_network_create(&network, AI_NETWORK_DATA_CONFIG);
    if (err.type != AI_ERROR_NONE) {
        HAL_UART_Transmit(&huart2, (uint8_t*)"AI Init FAIL!\r\n", 15, 100);
    } else {
        ai_network_params params = {
            AI_NETWORK_DATA_WEIGHTS(ai_network_data_weights_get()),
            AI_NETWORK_DATA_ACTIVATIONS(activations)
        };
        ai_network_init(network, &params);
        ai_input = ai_network_inputs_get(network, NULL);
        ai_output = ai_network_outputs_get(network, NULL);
        HAL_UART_Transmit(&huart2, (uint8_t*)"AI Init OK!\r\n", 13, 100);
    }

	/* Power-on default: a cold boot (battery/USB just connected) does NOT
	   fully power the device - it flashes once to show it has power, then
	   goes to sleep (STOP) until the user holds PC6 for a long press. */
	Led_Blink(1, PWR_LED_BLINK_MS);
	PowerManager_SleepUntilLongPress();
	System_PowerOn();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	uint8_t result = 99, wAmI = 99;
	char btMsg[64];
  bool is_streaming = false;
  uint8_t rx_byte = 0;
		
	uint8_t sys_state = 0; 
  uint16_t frame_count = 0;

  int ai_best_class = 0;
  float ai_max_prob = 0.0f;
	bool is_collecting = false;
	uint32_t led_blink_tick = 0;
	bool bt_connected = false;
	char bt_notify_buf[BT_NOTIFY_BUF_LEN + 1] = {0};
	uint8_t bt_notify_len = 0;

  while (1)
  {
    /* USER CODE END WHILE */

  //MX_X_CUBE_AI_Process();
    /* USER CODE BEGIN 3 */
		/* Power button: a long press while running powers the device off
		   (IMU rail off + STOP sleep). System_PowerOff() blocks until the
		   device is woken and re-initialized, then returns here. */
		if (HAL_GPIO_ReadPin(PW_BTN_PC6_GPIO_Port, PW_BTN_PC6_Pin) == GPIO_PIN_SET) {
			if (PowerButton_HeldFor(PWR_LONG_PRESS_MS)) {
				System_PowerOff();
				/* discard any in-flight collection/AI state from before sleep */
				sys_state = 0;
				frame_count = 0;
				/* The module re-advertises after waking; don't assume the
				   old connection survived - wait for a fresh "OK+CONN". */
				bt_connected = false;
				bt_notify_len = 0;
				bt_notify_buf[0] = '\0';
			}
		}

		if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE)) {
				__HAL_UART_CLEAR_FLAG(&huart2, UART_CLEAR_OREF);
		}

		/* Drive PC7 as the BLE-link indicator: fast blink while not
		   connected, slow blink once the module has reported "OK+CONN". */
		{
			uint32_t blink_period = bt_connected ? BT_BLINK_SLOW_MS : BT_BLINK_FAST_MS;
			if (HAL_GetTick() - led_blink_tick >= blink_period) {
				HAL_GPIO_TogglePin(LED_control_GPIO_Port, LED_control_Pin);
				led_blink_tick = HAL_GetTick();
			}
		}

		if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE)) {
			HAL_UART_Receive(&huart2, &rx_byte, 1, 0);

			/* Feed a small rolling buffer to catch the HM19 module's own
			   unsolicited "OK+CONN" / "OK+LOST" notifications, wherever
			   they land relative to byte boundaries. */
			if (bt_notify_len >= BT_NOTIFY_BUF_LEN) {
				memmove(bt_notify_buf, bt_notify_buf + 1, BT_NOTIFY_BUF_LEN - 1);
				bt_notify_len = BT_NOTIFY_BUF_LEN - 1;
			}
			bt_notify_buf[bt_notify_len++] = (char)rx_byte;
			bt_notify_buf[bt_notify_len] = '\0';

			if (strstr(bt_notify_buf, BT_NOTIFY_CONNECTED) != NULL) {
				bt_connected = true;
				bt_notify_len = 0;
				bt_notify_buf[0] = '\0';
			} else if (strstr(bt_notify_buf, BT_NOTIFY_LOST) != NULL) {
				bt_connected = false;
				bt_notify_len = 0;
				bt_notify_buf[0] = '\0';
			}

			if (rx_byte == '0') {
				HAL_UART_Transmit(&huart2, (uint8_t*)"READY\r\n", 7, 100);
			}

			if (rx_byte == '1' && sys_state == 0) {
				sys_state = 1;
				frame_count = 0;
                HAL_UART_Transmit(&huart2, (uint8_t*)"MSG: Start Collecting...\r\n", 26, 100);
			} 
			else if (rx_byte == '2') {
                
                if (sys_state == 2) {
                    sys_state = 0; 
									
                    float *out_data = (float *)ai_output[0].data;
									
                    sprintf(btMsg, "RESULT:stage_%d|%.4f,%.4f,%.4f\r\n", 
                            ai_best_class, out_data[0], out_data[1], out_data[2]);
									
                    HAL_UART_Transmit(&huart2, (uint8_t*)btMsg, strlen(btMsg), 100);
                    
                    HAL_UART_Transmit(&huart2, (uint8_t*)"MSG: Dumping Processed Data...\r\n", 32, 100);
                    for (int i = 0; i < WINDOW_FRAMES; i++) {
                        sprintf(btMsg, "D:%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\r\n", 
                                processed_buffer[i][0], processed_buffer[i][1], processed_buffer[i][2],
                                processed_buffer[i][3], processed_buffer[i][4], processed_buffer[i][5]);
                        HAL_UART_Transmit(&huart2, (uint8_t*)btMsg, strlen(btMsg), 100);
                        HAL_Delay(5); 
                    }
                    HAL_UART_Transmit(&huart2, (uint8_t*)"MSG: Dump Done.\r\n", 17, 100);
                } 
                else {
                    sprintf(btMsg, "Wait! Im not ready. State: %d\r\n", sys_state);
                    HAL_UART_Transmit(&huart2, (uint8_t*)btMsg, strlen(btMsg), 100);
                }
			}
		}

		if (IsIcmRawDataRdy(&g_icmCb)) {
			
			GetIcmAccelRawData(&g_icmCb, &my_accel);
			GetIcmGyroRawData(&g_icmCb, &my_gyro);
			
			if (sys_state == 1) {
                raw_data_buffer[frame_count][0] = (float)my_accel.x;
                raw_data_buffer[frame_count][1] = (float)my_accel.y;
                raw_data_buffer[frame_count][2] = (float)my_accel.z;
                raw_data_buffer[frame_count][3] = (float)my_gyro.x;
                raw_data_buffer[frame_count][4] = (float)my_gyro.y;
                raw_data_buffer[frame_count][5] = (float)my_gyro.z;
                
                frame_count++;
				
				if (frame_count >= WINDOW_FRAMES) {
                    
                    HAL_UART_Transmit(&huart2, (uint8_t*)"MSG: Processing ETL & AI...\r\n", 29, 100);
					
					Apply_ETL();
					
					memcpy(ai_input[0].data, processed_buffer, sizeof(float) * WINDOW_FRAMES * NUM_AXES);
					
					ai_i32 n_batch;
                    n_batch = ai_network_run(network, &ai_input[0], &ai_output[0]);
					
					if (n_batch > 0) {
                        float *out_data = (float *)ai_output[0].data;
                        ai_max_prob = out_data[0];
                        ai_best_class = 0;

                        for (int c = 1; c < 3; c++) {
                            if (out_data[c] > ai_max_prob) {
                                ai_max_prob = out_data[c];
                                ai_best_class = c + 1;
                            }
                        }

													sys_state = 2;
													HAL_UART_Transmit(&huart2, (uint8_t*)"TT_OK\r\n", 7, 100);

                    } else {
                        HAL_UART_Transmit(&huart2, (uint8_t*)"AI Run Error!\r\n", 15, 100);
												sys_state = 0; 
                    }
                }
            }
		}
		
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_6) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HM19_SendATCommand(const char* cmd, char* response, uint16_t respLen)
{
  memset(response, 0, respLen);
  
  HAL_UART_Transmit(&huart2, (uint8_t*)cmd, strlen(cmd), HAL_MAX_DELAY);
  
  HAL_UART_Receive(&huart2, (uint8_t*)response, respLen - 1, 200);
}

void HM19_ConfigureAsSlaveWithName()
{
  char response[64];

  HM19_SendATCommand("AT+NAMEBT-TEST_746", response, sizeof(response));
  HAL_Delay(500);

  HM19_SendATCommand("AT+RESET", response, sizeof(response));
  HAL_Delay(1000);
}

static void Led_Blink(uint8_t times, uint32_t period_ms)
{
	for (uint8_t i = 0; i < times; i++) {
		LED_ON();
		HAL_Delay(period_ms);
		LED_OFF();
		HAL_Delay(period_ms);
	}
}

/* Blocks while PC6 is held down. Returns true once it has been held
   continuously for at least `ms`, or false if released earlier. */
static bool PowerButton_HeldFor(uint32_t ms)
{
	uint32_t start = HAL_GetTick();
	while (HAL_GPIO_ReadPin(PW_BTN_PC6_GPIO_Port, PW_BTN_PC6_Pin) == GPIO_PIN_SET) {
		if (HAL_GetTick() - start >= ms) {
			return true;
		}
	}
	return false;
}

/* Enters STOP mode and stays there - waking briefly on every PC6 press -
   until a press is held long enough to count as a deliberate long press.
   Short/accidental presses just put the MCU straight back to sleep. */
static void PowerManager_SleepUntilLongPress(void)
{
	for (;;) {
		/* Wait for release before sleeping so a still-held button doesn't
		   leave the EXTI pending flag set (which would wake immediately). */
		while (HAL_GPIO_ReadPin(PW_BTN_PC6_GPIO_Port, PW_BTN_PC6_Pin) == GPIO_PIN_SET) { }
		__HAL_GPIO_EXTI_CLEAR_IT(PW_BTN_PC6_Pin);

		/* Belt-and-braces: guarantee the LED is dark for the sleep itself,
		   regardless of whatever state earlier code left it in. */
		LED_OFF();

		/* Stop the 1ms SysTick interrupt before sleeping - if it were still
		   enabled, a tick pending right as WFI executes can abort/shorten
		   the sleep (classic STOP-mode gotcha), so this is disabled for the
		   duration and only re-enabled once the clock is back to full speed. */
		HAL_SuspendTick();
		HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

		/* --- execution resumes here once PC6 wakes the MCU --- */
		SystemClock_Config(); /* STOP mode falls back to HSI; restore full speed */
		HAL_ResumeTick();

		if (PowerButton_HeldFor(PWR_LONG_PRESS_MS)) {
			return;
		}
		/* Too short to count - treat as noise/accidental touch and re-sleep. */
	}
}

/* Powers the IMU rail back on and re-initializes the sensor (it lost power
   while asleep). Returns whether the IMU responded. */
static bool System_PowerOn(void)
{
	HAL_GPIO_WritePin(Pin1V8_GPIO_Port, Pin1V8_Pin, GPIO_PIN_SET);
	Led_Blink(2, PWR_LED_BLINK_MS);

	bool imuReady = IcmInit(&g_icmCb);
	if (imuReady) {
		IcmConfig(&g_icmCb);
		HAL_UART_Transmit(&huart2, (uint8_t*)"IMU Init OK!\r\n", 14, 100);
	} else {
		HAL_UART_Transmit(&huart2, (uint8_t*)"IMU Init FAIL!\r\n", 16, 100);
	}

	LED_OFF();
	return imuReady;
}

/* Cuts IMU power and puts the MCU to sleep. Note: the MCU/BT 3V3 rail
   cannot be switched off in hardware, so this is a low-power sleep, not a
   true full power-off. Blocks until woken by a long press, then leaves
   the device fully powered back on before returning. */
static void System_PowerOff(void)
{
	HAL_UART_Transmit(&huart2, (uint8_t*)"MSG: Powering off...\r\n", 23, 100);
	Led_Blink(3, PWR_LED_BLINK_MS);

	HAL_GPIO_WritePin(Pin1V8_GPIO_Port, Pin1V8_Pin, GPIO_PIN_RESET);
	LED_OFF();

	PowerManager_SleepUntilLongPress();

	System_PowerOn();
}

uint8_t IcmRead(IcmCb_t *pIcmCb ,uint8_t bank, uint8_t regAddr, uint8_t *pRegData, uint8_t length){
	uint8_t result = 0;
	
	if(pIcmCb->currRegBank != bank) {
		uint8_t bankData = bank << 4;
		result |= ICM_I2C_WRITE(0x7F, &bankData, 1);
		pIcmCb->currRegBank = bank;
		//HAL_Delay(10);
	}
	
	result |= ICM_I2C_READ(regAddr, pRegData, length);
	ICM_ASSERT(result != 0x00);
	//HAL_Delay(10);
	return result;
}

uint8_t IcmWrite(IcmCb_t *pIcmCb ,uint8_t bank, uint8_t regAddr, uint8_t *pRegData, uint8_t length){
	uint8_t result = 0;
	
	if(pIcmCb->currRegBank != bank) {
		uint8_t bankData = bank << 4;
		result |= ICM_I2C_WRITE(0x7F, &bankData, 1);
		pIcmCb->currRegBank = bank;
		//HAL_Delay(10);
	}
	result |= ICM_I2C_WRITE(regAddr, pRegData, length);
	ICM_ASSERT(result != 0x00);
	//HAL_Delay(10);
	return result;
}

void IcmConfig(IcmCb_t *pIcmCb){
	uint8_t raw_int_en = 0x01;

	IcmWrite(pIcmCb, 2, 0x14, (uint8_t []){0x19}, 1); 

	IcmWrite(pIcmCb, 2, 0x01, (uint8_t []){0x11}, 1); 
	
	IcmWrite(pIcmCb, 2, 0x00, (uint8_t []){0x0C}, 1); // GYRO_DIV
	IcmWrite(pIcmCb, 2, 0x10, (uint8_t []){0x00}, 1); // ACCEL_DIV_H
	IcmWrite(pIcmCb, 2, 0x11, (uint8_t []){0x0C}, 1); // ACCEL_DIV_L

	IcmWrite(pIcmCb, 0, 0x11, &raw_int_en, 1); 
}

bool IcmInit(IcmCb_t *pIcmCb){
	uint8_t result = 99, wAmI = 99;
	uint8_t retryCount = 0;
	pIcmCb->currRegBank = 0;
	
	while(retryCount < 5) {
		result = ICM_I2C_READ(0x00, &wAmI, 1);
		if(result == 0x00 && wAmI == 0xEA) {
			break;
		}
		retryCount++;
		HAL_Delay(100);
	}
	
	if(retryCount >= 5) {
		return false;
	}
	
	ICM_I2C_WRITE(0x06, (uint8_t[]){0x81}, 1); // Reset
	HAL_Delay(200);
	
	ICM_I2C_WRITE(0x06, (uint8_t[]){0x01}, 1); // Wake up
	HAL_Delay(50);

	return true; 
}

bool IsIcmRawDataRdy(IcmCb_t *pIcmCb){
	uint8_t status;
	IcmRead(pIcmCb, 0, 0x1A, &status, 1);
	return (status & 0x01)?true:false;
}

void GetIcmGyroRawData(IcmCb_t *pIcmCb, axises3* data){
	
	uint16_t gx, gy ,gz;
	
	uint8_t gyroDataArray[6];
	
	IcmRead(pIcmCb, 0, 0x33, gyroDataArray, 6);
	
	gx = gyroDataArray[0] << 8 | gyroDataArray[1];
	gy = gyroDataArray[2] << 8 | gyroDataArray[3];
	gz = gyroDataArray[4] << 8 | gyroDataArray[5];
	data->x = (int16_t)(gyroDataArray[0] << 8 | gyroDataArray[1]);
	data->y = (int16_t)(gyroDataArray[2] << 8 | gyroDataArray[3]);
	data->z = (int16_t)(gyroDataArray[4] << 8 | gyroDataArray[5]);
	data->ux = (int16_t)(gyroDataArray[0] << 8 | gyroDataArray[1]);
	data->uy = (int16_t)(gyroDataArray[2] << 8 | gyroDataArray[3]);
	data->uz = (int16_t)(gyroDataArray[4] << 8 | gyroDataArray[5]);
	
//	char debugMsg[100];
//	sprintf(debugMsg, "GX: %-5d, GY: %-5d, GZ: %-5d\r\n", (int16_t)(gx), (int16_t)(gy), (int16_t)(gz));
//	HAL_UART_Transmit(&huart2, (uint8_t*)debugMsg, strlen(debugMsg), HAL_MAX_DELAY);
	
	//printf("GX: %-5d, GY: %-5d, GZ: %-5d\n", (int16_t)(gx), (int16_t)gy, (int16_t)gz);
}

void GetIcmAccelRawData(IcmCb_t *pIcmCb, axises3* data){
	uint16_t ax, ay ,az;
	
	uint8_t accelDataArray[6];
	
	IcmRead(pIcmCb, 0, 0x2D, accelDataArray, 6);
	
	ax = accelDataArray[0] << 8 | accelDataArray[1];
	ay = accelDataArray[2] << 8 | accelDataArray[3];
	az = accelDataArray[4] << 8 | accelDataArray[5];
	data->x = (int16_t)(accelDataArray[0] << 8 | accelDataArray[1]);
	data->y = (int16_t)(accelDataArray[2] << 8 | accelDataArray[3]);
	data->z = (int16_t)(accelDataArray[4] << 8 | accelDataArray[5]);
	data->ux = (int16_t)(accelDataArray[0] << 8 | accelDataArray[1]);
	data->uy = (int16_t)(accelDataArray[2] << 8 | accelDataArray[3]);
	data->uz = (int16_t)(accelDataArray[4] << 8 | accelDataArray[5]);
	
//	char debugMsg[100];
//	sprintf(debugMsg, "AX: %-5d, AY: %-5d, AZ: %-5d\r\n", (int16_t)(ax), (int16_t)(ay), (int16_t)(az));
//	HAL_UART_Transmit(&huart2, (uint8_t*)debugMsg, strlen(debugMsg), HAL_MAX_DELAY);
	
	//printf("AX: %-5d, AY: %-5d, AZ: %-5d\n", (int16_t)(ax), (int16_t)ay, (int16_t)az);
}

float temp[WINDOW_FRAMES];

void Apply_ETL() {
    for (int axis = 0; axis < NUM_AXES; axis++) {
        float init_val = raw_data_buffer[0][axis];
        float x1 = init_val, x2 = init_val;
        float y1 = init_val, y2 = init_val;
        
        float px1 = init_val, px2 = init_val;
        float py1 = init_val, py2 = init_val;
        
        for (int i = 0; i < WINDOW_FRAMES; i++) {
            float x = raw_data_buffer[i][axis];
            float y = butterworth_biquad_coeffs[0]*x + butterworth_biquad_coeffs[1]*x1 + butterworth_biquad_coeffs[2]*x2 + butterworth_biquad_coeffs[3]*y1 + butterworth_biquad_coeffs[4]*y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            
            float out = butterworth_biquad_coeffs[5]*y + butterworth_biquad_coeffs[6]*px1 + butterworth_biquad_coeffs[7]*px2 + butterworth_biquad_coeffs[8]*py1 + butterworth_biquad_coeffs[9]*py2;
            px2 = px1; px1 = y;
            py2 = py1; py1 = out;
            
            processed_buffer[i][axis] = out;
        }
    }

    for (int axis = 0; axis < NUM_AXES; axis++) {
        for (int i = 0; i < WINDOW_FRAMES; i++) {
            float sum = 0; int count = 0;
            for(int j = i - 2; j <= i + 2; j++) {
                if(j >= 0 && j < WINDOW_FRAMES) { 
                    sum += processed_buffer[j][axis]; 
                    count++; 
                }
            }
            temp[i] = sum / count;
        }
        for (int i = 0; i < WINDOW_FRAMES; i++) {
            processed_buffer[i][axis] = temp[i];
        }
    }
}
/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
