 /***************************************************************************//**
 * @file nvic-config.h
 * @brief NVIC Config Header
 * @version 0.01.0
 *******************************************************************************
 * @section License
 * <b>(C) Copyright 2014 Silicon Labs, http://www.silabs.com</b>
 *******************************************************************************
 *
 * This file is licensed under the Silabs License Agreement. See the file
 * "Silabs_License_Agreement.txt" for details. Before using this software for
 * any purpose, you must agree to the terms of that agreement.
 *

 ******************************************************************************/

#if (defined CORTEXM3_EZR32LG)
    #include "ezr32lg/nvic-config.h"
#elif (defined CORTEXM3_EZR32WG)
    #include "ezr32wg/nvic-config.h"
#elif (defined CORTEXM3_EZR32HG)
    #include "ezr32hg/nvic-config.h"
#elif defined (_EFR_DEVICE) || defined (CORTEXM3_EFR32_MICRO)
    #include "efr32/nvic-config.h"
#else
    #error Unknown EFM32 micro
#endif
