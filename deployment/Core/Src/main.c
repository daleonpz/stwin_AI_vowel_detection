/**
 ******************************************************************************
 * @file    main.c
 * @author  System Research & Applications Team - Catania Lab.
 * @version V2.4.0
 * @date    07-June-2021
 * @brief   Main program body
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) 2021 STMicroelectronics.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed by ST under Ultimate Liberty license
 * SLA0055, the "License"; You may not use this file except in compliance with
 * the License. You may obtain a copy of the License at:
 *                             www.st.com/SLA0055
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include <stdio.h>
#include <limits.h>
#include "main.h"
#include "hci.h"

#include "model.h"
#include "model_data.h"

/* Private define ------------------------------------------------------------*/
#define RING_BUFFER_SIZE    (600*6) /* 600 samples for each axis */
#define SAMPLES_PER_SEC     (1)   /* 1 sample per second */
#define WAIT_N_SAMPLES      (FREQ_ACC_GYRO_MAG / SAMPLES_PER_SEC)  
#define TENSOR_INPUT_SIZE   (20*20*6)
#define NUM_OF_SAMPLES      (TENSOR_INPUT_SIZE/ 6) /* 6 bytes per sample */
#define INFERENCE_THRESHOLD (0.8)

/* Imported Variables -------------------------------------------------------------*/

//#ifdef PREDMNT1_ENABLE_PRINTF
//extern TIM_HandleTypeDef  TimHandle;
//extern void CDC_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim);
//#endif /* PREDMNT1_ENABLE_PRINTF */


/* Exported Variables -------------------------------------------------------------*/
volatile uint32_t HCI_ProcessEvent=      0;
volatile uint8_t FifoEnabled = 1;

uint32_t ConnectionBleStatus  =0;

TIM_HandleTypeDef    TimCCHandle;

uint32_t uhCCR1_Val = DEFAULT_uhCCR1_Val;
uint32_t uhCCR2_Val = DEFAULT_uhCCR2_Val;
uint32_t uhCCR3_Val = DEFAULT_uhCCR3_Val;
uint32_t uhCCR4_Val = DEFAULT_uhCCR4_Val;

uint8_t  NodeName[8];

/* Private variables ---------------------------------------------------------*/
uint16_t VibrationParam[11];
CRC_HandleTypeDef hcrc;

static ai_handle model = AI_HANDLE_NULL;

AI_ALIGNED(32)
// static ai_float activations[AI_MODEL_DATA_ACTIVATIONS_SIZE];
static ai_u8 activations[AI_MODEL_DATA_ACTIVATIONS_SIZE];
AI_ALIGNED(32)
static ai_float in_data[AI_MODEL_IN_1_SIZE];
AI_ALIGNED(32)
static ai_float out_data[AI_MODEL_OUT_1_SIZE];

/* Array of pointer to manage the model's input/output tensors */
static ai_buffer *ai_input;
static ai_buffer *ai_output;


static int32_t _ring_buffer[RING_BUFFER_SIZE];
static uint32_t _ring_buffer_index = 0;


/* Table with All the known Meta Data */
MDM_knownGMD_t known_MetaData[]={
    {GMD_NODE_NAME,      (sizeof(NodeName))},
    {GMD_VIBRATION_PARAM,(sizeof(VibrationParam))},
    {GMD_END    ,0}/* THIS MUST BE THE LAST ONE */
};

static volatile uint32_t t_stwin=               0;

static volatile uint8_t  g_led_on           = 0;
static volatile uint32_t g_acc_counter      = 0;
// static volatile uint8_t  g_is_data_ready    = 0;

/* Private function prototypes -----------------------------------------------*/
static void SystemClock_Config(void);

static void InitTimers(void);
static void InitPredictiveMaintenance(void);

static void ReadMotionData(void);

static void Enable_Inertial_Timer(void);
static void MX_CRC_Init(void);

// create a ring buffer for the IMU data
static void ring_buffer_init(void);
static void ring_buffer_add(int32_t *data);
static void ring_buffer_print_all(void);

// AI framework related functions
static int aiInit(void);
static int aiRun(const void *in_data, void *out_data);
static int aiAdquireAndProcessData(void *in_data);
static int aiPostProcessData(void *out_data);

/* Private functions ---------------------------------------------------------*/
static void ring_buffer_init(void)
{
    _ring_buffer_index = 0;
}


static void ring_buffer_add(int32_t *data)
{
    for (int i = 0; i < 6; i++) {
        _ring_buffer[_ring_buffer_index + i] = data[i];
    }
//     _PRINTF("ring_buffer_add: %d %d %d %d %d %d , index: %d \r\n ", 
//             _ring_buffer[_ring_buffer_index], _ring_buffer[_ring_buffer_index + 1], _ring_buffer[_ring_buffer_index + 2],
//             _ring_buffer[_ring_buffer_index + 3], _ring_buffer[_ring_buffer_index + 4], _ring_buffer[_ring_buffer_index + 5],
//             _ring_buffer_index);
//     _PRINTF("%ld, %ld, %ld, %ld, %ld %ld\r\n",
//             _ring_buffer[_ring_buffer_index], _ring_buffer[_ring_buffer_index + 1], _ring_buffer[_ring_buffer_index + 2],
//             _ring_buffer[_ring_buffer_index + 3], _ring_buffer[_ring_buffer_index + 4], _ring_buffer[_ring_buffer_index + 5])
    _ring_buffer_index = (_ring_buffer_index + 6) % RING_BUFFER_SIZE;
}

static void ring_buffer_print_all(void)
{
    int i;
    _PRINTF("ring buffer: \r\n");
    for (i = 0; i < RING_BUFFER_SIZE; i++) {
        _PRINTF("%ld ", _ring_buffer[i]);
    }
    _PRINTF("\r\n (ring buffer size: %i)\r\n", RING_BUFFER_SIZE);
    _PRINTF(" index: %lu\r\n", _ring_buffer_index);
}

static int aiInit(void) 
{
    ai_error err;

    /* Create and initialize the c-model */
    const ai_handle acts[] = { activations };
    err = ai_model_create_and_init(&model, acts, NULL);
    if (err.type != AI_ERROR_NONE) { 
        _PRINTF("ai_model_create_and_init failed. Error type: %d  code: %d \r\n", err.type, err.code);
        while(1);
    };

    /* Reteive pointers to the model's input/output tensors */
    ai_input    = ai_model_inputs_get(model, NULL);
    ai_output   = ai_model_outputs_get(model, NULL);

    return 0;
}

static int aiRun(const void *in_data, void *out_data) 
{
    ai_i32 n_batch;
    ai_error err;

    /* 1 - Update IO handlers with the data payload */
    ai_input[0].data = AI_HANDLE_PTR(in_data);
    ai_output[0].data = AI_HANDLE_PTR(out_data);

    /* 2 - Perform the inference */
    n_batch = ai_model_run(model, &ai_input[0], &ai_output[0]);
    if (n_batch != 1) 
    {
        err = ai_model_get_error(model);
        if (err.type != AI_ERROR_NONE) 
        {
            _PRINTF("ai_model_run failed. error type: %d  code: %d \r\n", err.type, err.code);
            while(1);
        }
    };

    return 0;
}

static int aiAdquireAndProcessData(void *in_data)
{
    ai_float *data = (ai_float *)in_data;
    ai_float min[6] = {INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX};
    ai_float max[6] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};

//     _PRINTF("Adquiring data...\r\n");
    for (int i = 0; i < NUM_OF_SAMPLES; i ++) 
    {
        const int index = (_ring_buffer_index + i*6) % RING_BUFFER_SIZE;
        const int32_t* value = &(_ring_buffer[index]);

        for (int j = 0; j < 6; j++) 
        {
            data[i*6 + j] = (ai_float)(value[j]);
            min[j] = MIN(min[j], data[i*6 + j]);
            max[j] = MAX(max[j], data[i*6 + j]);
        }
        
        const int idx = i*6;
//        _PRINTF("[> %i] %f %f %f %f %f %f\r\n ", i, data[idx], data[idx + 1], data[idx + 2], data[idx + 3], data[idx + 4], data[idx + 5]);
//         _PRINTF("[%i] %ld %ld %ld %ld %ld %ld\r\n ", i, value[0], value[1], value[2], value[3], value[4], value[5]);
    }

//     _PRINTF("min: %f %f %f %f %f %f \r\n", min[0], min[1], min[2], min[3], min[4], min[5]);
//     _PRINTF("max: %f %f %f %f %f %f \r\n", max[0], max[1], max[2], max[3], max[4], max[5]);

    // normalize the float array
//     _PRINTF("Normalizing data...\r\n");
    for (int i = 0; i < NUM_OF_SAMPLES; i ++) 
    {
        const int idx = i*6;
        for (int j = 0; j < 6; j++) 
        {
            ai_float max_min = max[j] - min[j];
            if (max_min == 0) {
                data[idx + j] = 0;
            } else {
                data[idx + j] = (data[idx + j] - min[j]) / max_min;
            }
        }
//         _PRINTF("[> %i] %f %f %f %f %f %f\r\n ", i, data[idx], data[idx + 1], data[idx + 2], data[idx + 3], data[idx + 4], data[idx + 5]);
    }

    return 0;

}

static int aiPostProcessData(void *out_data)
{
    ai_float *data = (ai_float *)out_data;
    
    // print float array
    _PRINTF("inference data: \r\n");
    _PRINTF("%f \r\n", data[0]);
    _PRINTF("%f \r\n", data[1]);

    // find the max value
    ai_float max = INT32_MIN;
    int max_index = 0;
    for (int i = 0; i < AI_MODEL_OUT_1_SIZE; i++) {
        if (data[i] > max) {
            max = data[i];
            max_index = i;
        }
    }

    if ( INFERENCE_THRESHOLD < max ) {
        _PRINTF("inference result: %i \r\n", max_index);
    } else {
        _PRINTF("inference result: unknown \r\n");
        max_index = 99;
    }

    char *gesture = "unknown";
    switch(max_index){
        case 0:
            gesture = "A";
            break;
        case 1:
            gesture = "E";
            break;
        case 2:
            gesture = "I";
            break;
        case 3:
            gesture = "O";
            break;
        case 4:
            gesture = "U";
            break;
        default:
            break;
    }
    _PRINTF("gesture: %s \r\n", gesture);

    return 0;
}


/**
 * @brief  Main program
 * @param  None
 * @retval None
 */
int main(void)
{
    HAL_Init();

    HAL_PWREx_EnableVddIO2();
    __HAL_RCC_PWR_CLK_ENABLE();
  /* The STM32 CRC IP clock should be enabled to use the network runtime library */
//     __HAL_RCC_CRS_CLK_ENABLE();
    HAL_PWREx_EnableVddUSB();

    /* Configure the System clock */
    SystemClock_Config();

    InitTargetPlatform();

    t_stwin = HAL_GetTick();

    MX_CRC_Init();

    /* Check the MetaDataManager */
    InitMetaDataManager((void *)&known_MetaData,MDM_DATA_TYPE_GMD,NULL); 

//     _PRINTF("\n\t(HAL %ld.%ld.%ld_%ld)\r\n"
//             "\tCompiled %s %s"
// 
// #if defined (__IAR_SYSTEMS_ICC__)
//             " (IAR)\r\n"
// #elif defined (__CC_ARM)
//             " (KEIL)\r\n"
// #elif defined (__GNUC__)
//             " (STM32CubeIDE)\r\n"
// #endif
//             "\tSend Every %4dmS Acc/Gyro/Magneto\r\n",
//             HAL_GetHalVersion() >>24,
//             (HAL_GetHalVersion() >>16)&0xFF,
//             (HAL_GetHalVersion() >> 8)&0xFF,
//             HAL_GetHalVersion()      &0xFF,
//             __DATE__,__TIME__,
//             ALGO_PERIOD_ACC_GYRO_MAG);
// 

    HCI_TL_SPI_Reset();

    /* initialize timers */
    InitTimers();

    /* Predictive Maintenance Initialization */
//     InitPredictiveMaintenance();
    
    aiInit();
    ring_buffer_init();

    Enable_Inertial_Timer();   

    /* Infinite loop */
    while (1)
    {
        
        if (WAIT_N_SAMPLES*3 == g_acc_counter){
            uint32_t tick_start  = 0;
            uint32_t tick_end    = 0;
            g_led_on ^= 1;
            g_acc_counter = 0;
            _PRINTF("--------------------\r\n");
            _PRINTF("RUNNING AI MODEL\r\n");
            aiAdquireAndProcessData(in_data);
            tick_start  = HAL_GetTick();
            aiRun(in_data, out_data);
            tick_end    = HAL_GetTick();
            _PRINTF("inference time: %ld ms\r\n", tick_end - tick_start);
            aiPostProcessData(out_data);
            _PRINTF("MOVE NOWWWW!!!!!!\r\n");
        }

//         if (g_is_data_ready){
//             ReadMotionData();
//         }

        if ( !g_led_on ) {
           LedOnTargetPlatform();
        }
        else{
            LedOffTargetPlatform();
        }

    }
}

static void MX_CRC_Init(void)
{
    hcrc.Instance = CRC;
    hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
    hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
    hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
    hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
    hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
    if (HAL_CRC_Init(&hcrc) != HAL_OK)
    {
        Error_Handler();
    }
}
/**
 * @brief  Send Motion Data Acc/Mag/Gyro to BLE
 * @param  None
 * @retval None
 */
static void ReadMotionData(void)
{
    MOTION_SENSOR_Axes_t ACC_Value;
    MOTION_SENSOR_Axes_t GYR_Value;
    MOTION_SENSOR_Axes_t MAG_Value;

    /* Reset the Acc values */
    ACC_Value.x = ACC_Value.y = ACC_Value.z =0;

    /* Reset the Gyro values */
    GYR_Value.x = GYR_Value.y = GYR_Value.z =0;

    /* Reset the Magneto values */
    MAG_Value.x = MAG_Value.y = MAG_Value.z =0;

    TargetBoardFeatures.AccSensorIsInit = 1;
    TargetBoardFeatures.GyroSensorIsInit= 1;
    TargetBoardFeatures.MagSensorIsInit = 0;


    int32_t IMU_data[6] = {0};

//     _PRINTF("----------------------\r\n");
    /* Read the Acc values */
    if(TargetBoardFeatures.AccSensorIsInit)
    {
        MOTION_SENSOR_GetAxes(ACCELERO_INSTANCE, MOTION_ACCELERO, &ACC_Value);
        IMU_data[0] = ACC_Value.x;
        IMU_data[1] = ACC_Value.y;
        IMU_data[2] = ACC_Value.z;
//         _PRINTF("%ld, %ld, %ld", ACC_Value.x, ACC_Value.y, ACC_Value.z);;

    }

    /* Read the Gyro values */
    if(TargetBoardFeatures.GyroSensorIsInit)
    {
        MOTION_SENSOR_GetAxes(GYRO_INSTANCE,MOTION_GYRO, &GYR_Value);
        IMU_data[3] = GYR_Value.x;
        IMU_data[4] = GYR_Value.y;
        IMU_data[5] = GYR_Value.z;
//         _PRINTF(", %ld, %ld, %ld \r\n", GYR_Value.x, GYR_Value.y, GYR_Value.z);;

    }

    ring_buffer_add(IMU_data);
//     ring_buffer_print_all();

    /* Read the Magneto values */
    if(TargetBoardFeatures.MagSensorIsInit)
    {
        MOTION_SENSOR_GetAxes(MAGNETO_INSTANCE, MOTION_MAGNETO, &MAG_Value);
        _PRINTF("%ld, %ld, %ld ", MAG_Value.x, MAG_Value.y, MAG_Value.z);;

    }
}


/**
 * @brief  Function for initializing timers for sending the information to BLE:
 *  - 1 for sending MotionFX/AR/CP and Acc/Gyro/Mag
 *  - 1 for sending the Environmental info
 * @param  None
 * @retval None
 */
static void InitTimers(void)
{
    uint32_t uwPrescalerValue;

    /* Timer Output Compare Configuration Structure declaration */
    TIM_OC_InitTypeDef sConfig;

    /* Compute the prescaler value to have TIM1 counter clock equal to 10 KHz */
    uwPrescalerValue = (uint32_t) ((SystemCoreClock / 10000) - 1); 

    _PRINTF("system clock ----> %lu\r\n", SystemCoreClock);

    /* Set TIM1 instance ( Motion ) */
    TimCCHandle.Instance = TIM1;  
    TimCCHandle.Init.Period        = 65535;
    TimCCHandle.Init.Prescaler     = uwPrescalerValue;
    TimCCHandle.Init.ClockDivision = 0;
    TimCCHandle.Init.CounterMode   = TIM_COUNTERMODE_UP;
    if(HAL_TIM_OC_Init(&TimCCHandle) != HAL_OK)
    {
        /* Initialization Error */
        Error_Handler();
    }

    /* Configure the Output Compare channels */
    /* Common configuration for all channels */
    sConfig.OCMode     = TIM_OCMODE_TOGGLE;
    sConfig.OCPolarity = TIM_OCPOLARITY_LOW;

    /* Output Compare Toggle Mode configuration: Channel2 for environmental sensor */
    sConfig.Pulse = DEFAULT_uhCCR1_Val;
    if(HAL_TIM_OC_ConfigChannel(&TimCCHandle, &sConfig, TIM_CHANNEL_1) != HAL_OK)
    {
        /* Configuration Error */
        Error_Handler();
    }

    /* Output Compare Toggle Mode configuration: Channel2 for mic audio level */
    sConfig.Pulse = DEFAULT_uhCCR2_Val;
    if(HAL_TIM_OC_ConfigChannel(&TimCCHandle, &sConfig, TIM_CHANNEL_2) != HAL_OK)
    {
        /* Configuration Error */
        Error_Handler();
    }

    /* Output Compare Toggle Mode configuration: Channel3 for Acc/Gyro/Mag sensor */
    sConfig.Pulse = DEFAULT_uhCCR3_Val; // 50ms
    if(HAL_TIM_OC_ConfigChannel(&TimCCHandle, &sConfig, TIM_CHANNEL_3) != HAL_OK)
    {
        /* Configuration Error */
        Error_Handler();
    }

    /* Output Compare Toggle Mode configuration: Channel4 for battery info */
    sConfig.Pulse = DEFAULT_uhCCR4_Val;
    if(HAL_TIM_OC_ConfigChannel(&TimCCHandle, &sConfig, TIM_CHANNEL_4) != HAL_OK)
    {
        /* Configuration Error */
        Error_Handler();
    }
}


/** @brief Predictive Maintenance Initialization
 * @param None
 * @retval None
 */
static void InitPredictiveMaintenance(void)
{
    /* Set the vibration parameters with default values */
    MotionSP_SetDefaultVibrationParam();

    _PRINTF("\r\nAccelerometer parameters:\r\n");
    _PRINTF("\r\n");

    _PRINTF("************************************************************************\r\n\r\n");

    /* Initializes accelerometer with vibration parameters values */
    if(MotionSP_AcceleroConfig()) {
        _PRINTF("\tFailed Set Accelerometer Parameters\r\n\n");
    } else {
        _PRINTF("\tOK Set Accelerometer Parameters\r\n\n");
    }
}

/**
 * @brief  System Clock Configuration
 *         The system Clock is configured as follow : 
 *            System Clock source            = PLL (HSI)
 *            SYSCLK(Hz)                     = 
 *            HCLK(Hz)                       = 
 *            AHB Prescaler                  = 1
 *            APB1 Prescaler                 = 1
 *            APB2 Prescaler                 = 1
 *            HSE Frequency(Hz)              = 
 *            PLL_M                          = 2
 *            PLL_N                          = 30
 *            PLL_P                          = 2
 *            PLL_Q                          = 2
 *            PLL_R                          = 2
 *            VDD(V)                         = 3.3
 *            Main regulator output voltage  = Scale1 mode
 *            Flash Latency(WS)              = 5
 * @param  None
 * @retval None
 */
void SystemClock_Config(void)
{
    //   __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    //  
    //  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    //  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    //  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

    RCC_OscInitTypeDef RCC_OscInitStruct;
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    RCC_PeriphCLKInitTypeDef PeriphClkInit;

    /* Configure the main internal regulator output voltage */
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK)
    {
        Error_Handler();
    }

    /* Initializes the CPU, AHB and APB busses clocks */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;  // External crystal   (32khz/16khz)
    RCC_OscInitStruct.HSI48State = RCC_HSI48_ON; //48 Mhz Source to drive usbe
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 2;
    RCC_OscInitStruct.PLL.PLLN = 30;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV5; //RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /* Initializes the CPU, AHB and APB busses clocks */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
        |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
    {
        Error_Handler();
    }

    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC
        |RCC_PERIPHCLK_DFSDM1
        |RCC_PERIPHCLK_USB
        |RCC_PERIPHCLK_ADC
        |RCC_PERIPHCLK_I2C2
        |RCC_PERIPHCLK_SAI1;
    PeriphClkInit.Sai1ClockSelection = RCC_SAI1CLKSOURCE_PLLSAI1;
    PeriphClkInit.I2c2ClockSelection = RCC_I2C2CLKSOURCE_PCLK1;
    PeriphClkInit.AdcClockSelection = RCC_ADCCLKSOURCE_PLLSAI1;
    PeriphClkInit.Dfsdm1ClockSelection = RCC_DFSDM1CLKSOURCE_PCLK2; //RCC_DFSDM1CLKSOURCE_PCLK;
    PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
    PeriphClkInit.PLLSAI1.PLLSAI1Source = RCC_PLLSOURCE_HSE;
    PeriphClkInit.PLLSAI1.PLLSAI1M = 5;
    PeriphClkInit.PLLSAI1.PLLSAI1N = 96;
    PeriphClkInit.PLLSAI1.PLLSAI1P = RCC_PLLP_DIV25;
    PeriphClkInit.PLLSAI1.PLLSAI1Q = RCC_PLLQ_DIV4;
    PeriphClkInit.PLLSAI1.PLLSAI1R = RCC_PLLR_DIV4;
    PeriphClkInit.PLLSAI1.PLLSAI1ClockOut = RCC_PLLSAI1_ADC1CLK|RCC_PLLSAI1_SAI1CLK;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
        Error_Handler();
    }

    /* Configure the Systick interrupt time */
    HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq()/1000);

    /* Configure the Systick */
    HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

    /* SysTick_IRQn interrupt configuration */
    HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

/*******************************************/
/* Hardware Characteristics Notify Service */
/*******************************************/
/**
 * @brief  Enable timer to generate periodic event 
 * * @param  None
 * @retval None
 */
static void Enable_Inertial_Timer(void)
{ 
    /* Start the TIM Base generation in interrupt mode (for Acc/Gyro/Mag sensor) */
    if(HAL_TIM_OC_Start_IT(&TimCCHandle, TIM_CHANNEL_3) != HAL_OK){
        /* Starting Error */
        Error_Handler();
    }

    /* Set the new Capture compare value */
    {
        uint32_t uhCapture = __HAL_TIM_GET_COUNTER(&TimCCHandle);
        /* Set the Capture Compare Register value (for Acc/Gyro/Mag sensor) */
        __HAL_TIM_SET_COMPARE(&TimCCHandle, TIM_CHANNEL_3, (uhCapture + uhCCR3_Val));
    }

}

/***********************************
 * Software Characteristics Service *
 ************************************/
/**
 * @brief  Output Compare callback in non blocking mode 
 * @param  htim : TIM OC handle
 * @retval None
 */
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{

    /* TIM1_CH3 toggling with frequency = 20 Hz */
    if(htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3)
    {
        uint32_t uhCapture;
        uhCapture = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_3);
        /* Set the Capture Compare Register value (for Acc/Gyro/Mag sensor) */
        __HAL_TIM_SET_COMPARE(&TimCCHandle, TIM_CHANNEL_3, (uhCapture + uhCCR3_Val));
        g_acc_counter++;
//         g_is_data_ready = 1;
        ReadMotionData();
    }

}

/**
 * @brief  Period elapsed callback in non blocking mode for Environmental timer
 * @param  htim : TIM handle
 * @retval None
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == STBC02_USED_TIM) {
        BC_CmdMng();
#ifdef PREDMNT1_ENABLE_PRINTF
    } else if(htim == (&TimHandle)) {
        CDC_TIM_PeriodElapsedCallback(htim);
#endif /* PREDMNT1_ENABLE_PRINTF */
    }
}

/**
 * @brief  Conversion complete callback in non blocking mode 
 * @param  htim : hadc handle
 * @retval None
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_3)
    {
        BSP_BC_ChgPinHasToggled();
    }
}
/**
 * @brief  EXTI line detection callback.
 * @param  uint16_t GPIO_Pin Specifies the pins connected EXTI line
 * @retval None
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{  
    switch(GPIO_Pin){
        //    case HCI_TL_SPI_EXTI_PIN:
        //      hci_tl_lowlevel_isr();
        //      HCI_ProcessEvent=1;
        //    break;
        case M_INT2_O_PIN:
            if(FifoEnabled)
                MotionSP_FifoFull_IRQ_Rtn();
            else
                MotionSP_DataReady_IRQ_Rtn();
            break;

        case GPIO_PIN_10:
            if(HAL_GetTick() - t_stwin > 4000)
            {
                BSP_BC_CmdSend(SHIPPING_MODE_ON);
            }
            break;
    }
}

/**
 * @brief This function provides accurate delay (in milliseconds) based 
 *        on variable incremented.
 * @note This is a user implementation using WFI state
 * @param Delay: specifies the delay time length, in milliseconds.
 * @retval None
 */
void HAL_Delay(__IO uint32_t Delay)
{
    uint32_t tickstart = 0;
    tickstart = HAL_GetTick();
    while((HAL_GetTick() - tickstart) < Delay){
        __WFI();
    }
}

/**
 * @brief  This function is executed in case of error occurrence.
 * @param  None
 * @retval None
 */
void Error_Handler(void)
{
    /* User may add here some code to deal with this error */
    while(1){
    }
}

/**
 * @}
 */

#ifdef  USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t* file, uint32_t line)
{ 
    /* User can add his own implementation to report the file name and line number,
ex: PREDMNT1_PRINTF("Wrong parameters value: file %s on line %d\r\n", file, line) */

    /* Infinite loop */
    while (1){
    }
}
#endif

/**
 * @}
 */

/**
 * @}
 */

/**
 * @}
 */

/**
 * @}
 */




/******************* (C) COPYRIGHT 2021 STMicroelectronics *****END OF FILE****/
