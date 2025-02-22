/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2019 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/mphal.h"
#include "irq.h"
#include "powerctrl.h"

#if defined(STM32WB)
void stm32_system_init(void) {
    if (RCC->CR == 0x00000560 && RCC->CFGR == 0x00070005) {
        // Wake from STANDBY with HSI enabled as system clock.  The second core likely
        // also needs HSI to remain enabled, so do as little as possible here.
        #if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
        // set CP10 and CP11 Full Access.
        SCB->CPACR |= (3 << (10 * 2)) | (3 << (11 * 2));
        #endif
        // Disable all interrupts.
        RCC->CIER = 0x00000000;
    } else {
        // Other start-up (eg POR), use standard system init code.
        SystemInit();
    }
}
#endif

void powerctrl_config_systick(void) {
    // Configure SYSTICK to run at 1kHz (1ms interval)
    SysTick->CTRL |= SYSTICK_CLKSOURCE_HCLK;
    SysTick_Config(HAL_RCC_GetHCLKFreq() / 1000);
    NVIC_SetPriority(SysTick_IRQn, IRQ_PRI_SYSTICK);

    #if !BUILDING_MBOOT && (defined(STM32H7) || defined(STM32L4) || defined(STM32WB))
    // Set SysTick IRQ priority variable in case the HAL needs to use it
    uwTickPrio = IRQ_PRI_SYSTICK;
    #endif
}

#if defined(STM32F0)

void SystemClock_Config(void) {
    // Enable power control peripheral
    __HAL_RCC_PWR_CLK_ENABLE();

    // Set flash latency to 1 because SYSCLK > 24MHz
    FLASH->ACR = (FLASH->ACR & ~0x7) | 0x1;

    #if MICROPY_HW_CLK_USE_HSI48
    // Use the 48MHz internal oscillator
    // HAL does not support RCC CFGR SW=3 (HSI48 direct to SYSCLK)
    // so use HSI48 -> PREDIV(divide by 2) -> PLL (mult by 2) -> SYSCLK.

    RCC->CR2 |= RCC_CR2_HSI48ON;
    while ((RCC->CR2 & RCC_CR2_HSI48RDY) == 0) {
        // Wait for HSI48 to be ready
    }
    RCC->CFGR = 0 << RCC_CFGR_PLLMUL_Pos | 3 << RCC_CFGR_PLLSRC_Pos; // PLL mult by 2, src = HSI48/PREDIV
    RCC->CFGR2 = 1; // Input clock divided by 2

    #elif MICROPY_HW_CLK_USE_HSE
    // Use HSE and the PLL to get a 48MHz SYSCLK

    #if MICROPY_HW_CLK_USE_BYPASS
    RCC->CR |= RCC_CR_HSEBYP;
    #endif
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0) {
        // Wait for HSE to be ready
    }
    RCC->CFGR = ((48000000 / HSE_VALUE) - 2) << RCC_CFGR_PLLMUL_Pos | 2 << RCC_CFGR_PLLSRC_Pos;
    RCC->CFGR2 = 0; // Input clock not divided

    #elif MICROPY_HW_CLK_USE_HSI
    // Use the 8MHz internal oscillator and the PLL to get a 48MHz SYSCLK

    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0) {
        // Wait for HSI to be ready
    }
    RCC->CFGR = 4 << RCC_CFGR_PLLMUL_Pos | 1 << RCC_CFGR_PLLSRC_Pos; // PLL mult by 6, src = HSI
    RCC->CFGR2 = 0; // Input clock not divided

    #else
    #error System clock not specified
    #endif

    RCC->CR |= RCC_CR_PLLON; // Turn PLL on
    while ((RCC->CR & RCC_CR_PLLRDY) == 0) {
        // Wait for PLL to lock
    }
    const uint32_t sysclk_src = 2;

    // Select SYSCLK source
    RCC->CFGR |= sysclk_src << RCC_CFGR_SW_Pos;
    while (((RCC->CFGR >> RCC_CFGR_SWS_Pos) & 0x3) != sysclk_src) {
        // Wait for SYSCLK source to change
    }

    SystemCoreClockUpdate();
    powerctrl_config_systick();
}

#elif defined(STM32G0)

void SystemClock_Config(void) {
    // Enable power control peripheral
    __HAL_RCC_PWR_CLK_ENABLE();

    // Set flash latency to 2 because SYSCLK > 48MHz
    FLASH->ACR = (FLASH->ACR & ~0x7) | 0x2;

    #if MICROPY_HW_CLK_USE_HSI
    // Enable the 16MHz internal oscillator and the PLL to get a 64MHz SYSCLK
    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0) {
        // Wait for HSI to be ready
    }

    // Use the PLL to get a 64MHz SYSCLK
    #define PLLM (HSI_VALUE / 16000000) // input is 8MHz
    #define PLLN (8) // 8*16MHz = 128MHz
    #define PLLP (2) // f_P = 64MHz
    #define PLLQ (2) // f_Q = 64MHz
    #define PLLR (2) // f_R = 64MHz
    RCC->PLLCFGR =
        (PLLP - 1) << RCC_PLLCFGR_PLLP_Pos | RCC_PLLCFGR_PLLPEN
            | (PLLQ - 1) << RCC_PLLCFGR_PLLQ_Pos | RCC_PLLCFGR_PLLQEN
            | (PLLR - 1) << RCC_PLLCFGR_PLLR_Pos | RCC_PLLCFGR_PLLREN
            | PLLN << RCC_PLLCFGR_PLLN_Pos
            | (PLLM - 1) << RCC_PLLCFGR_PLLM_Pos
            | RCC_PLLCFGR_PLLSRC_HSI;

    #else
    #error System clock not specified
    #endif

    RCC->CR |= RCC_CR_PLLON; // Turn PLL on
    while ((RCC->CR & RCC_CR_PLLRDY) == 0) {
        // Wait for PLL to lock
    }
    const uint32_t sysclk_src = 2; // 2 = PLLRCLK

    // Select SYSCLK source
    RCC->CFGR |= sysclk_src << RCC_CFGR_SW_Pos;
    while (((RCC->CFGR >> RCC_CFGR_SWS_Pos) & 0x7) != sysclk_src) {
        // Wait for SYSCLK source to change
    }

    SystemCoreClockUpdate();
    powerctrl_config_systick();

    #if MICROPY_HW_ENABLE_RNG || MICROPY_HW_ENABLE_USB
    // Enable the 48MHz internal oscillator
    RCC->CRRCR |= RCC_CRRCR_HSI48ON;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->CFGR3 |= SYSCFG_CFGR3_ENREF_HSI48;
    while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) {
        // Wait for HSI48 to be ready
    }

    // Select RC48 as HSI48 for USB and RNG
    RCC->CCIPR |= RCC_CCIPR_HSI48SEL;

    #if MICROPY_HW_ENABLE_USB
    // Synchronise HSI48 with 1kHz USB SoF
    __HAL_RCC_CRS_CLK_ENABLE();
    CRS->CR = 0x20 << CRS_CR_TRIM_Pos;
    CRS->CFGR = 2 << CRS_CFGR_SYNCSRC_Pos | 0x22 << CRS_CFGR_FELIM_Pos
        | __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000) << CRS_CFGR_RELOAD_Pos;
    #endif
    #endif
}

#elif defined(STM32L0)

void SystemClock_Config(void) {
    // Enable power control peripheral
    __HAL_RCC_PWR_CLK_ENABLE();

    // Set flash latency to 1 because SYSCLK > 16MHz
    FLASH->ACR |= FLASH_ACR_LATENCY;

    // Enable the 16MHz internal oscillator
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {
    }

    // Use HSI16 and the PLL to get a 32MHz SYSCLK
    RCC->CFGR = 1 << RCC_CFGR_PLLDIV_Pos | 1 << RCC_CFGR_PLLMUL_Pos;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
        // Wait for PLL to lock
    }
    const uint32_t sysclk_src = 3;

    // Select SYSCLK source
    RCC->CFGR |= sysclk_src << RCC_CFGR_SW_Pos;
    while (((RCC->CFGR >> RCC_CFGR_SWS_Pos) & 0x3) != sysclk_src) {
        // Wait for SYSCLK source to change
    }

    SystemCoreClockUpdate();
    powerctrl_config_systick();

    #if MICROPY_HW_ENABLE_RNG || MICROPY_HW_ENABLE_USB
    // Enable the 48MHz internal oscillator
    RCC->CRRCR |= RCC_CRRCR_HSI48ON;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->CFGR3 |= SYSCFG_CFGR3_ENREF_HSI48;
    while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) {
        // Wait for HSI48 to be ready
    }

    // Select RC48 as HSI48 for USB and RNG
    RCC->CCIPR |= RCC_CCIPR_HSI48SEL;

    #if MICROPY_HW_ENABLE_USB
    // Synchronise HSI48 with 1kHz USB SoF
    __HAL_RCC_CRS_CLK_ENABLE();
    CRS->CR = 0x20 << CRS_CR_TRIM_Pos;
    CRS->CFGR = 2 << CRS_CFGR_SYNCSRC_Pos | 0x22 << CRS_CFGR_FELIM_Pos
        | __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000) << CRS_CFGR_RELOAD_Pos;
    #endif
    #endif
}

#elif defined(STM32L1)

void SystemClock_Config(void) {
    // Enable power control peripheral
    __HAL_RCC_PWR_CLK_ENABLE();

    // Set power voltage scaling
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    // Enable the FLASH 64-bit access
    FLASH->ACR = FLASH_ACR_ACC64;
    // Set flash latency to 1 because SYSCLK > 16MHz
    FLASH->ACR |= MICROPY_HW_FLASH_LATENCY;

    #if MICROPY_HW_CLK_USE_HSI
    // Enable the 16MHz internal oscillator
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {
    }
    RCC->CFGR = RCC_CFGR_PLLSRC_HSI;
    #else
    // Enable the 8MHz external oscillator
    RCC->CR |= RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
    }
    RCC->CFGR = RCC_CFGR_PLLSRC_HSE;
    #endif
    // Use HSI16 and the PLL to get a 32MHz SYSCLK
    RCC->CFGR |= MICROPY_HW_CLK_PLLMUL | MICROPY_HW_CLK_PLLDIV;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
        // Wait for PLL to lock
    }
    RCC->CFGR |= RCC_CFGR_SW_PLL;

    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL) {
        // Wait for SYSCLK source to change
    }

    SystemCoreClockUpdate();
    powerctrl_config_systick();

    #if MICROPY_HW_ENABLE_USB
    // Enable the 48MHz internal oscillator
    RCC->CRRCR |= RCC_CRRCR_HSI48ON;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->CFGR3 |= SYSCFG_CFGR3_ENREF_HSI48;
    while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) {
        // Wait for HSI48 to be ready
    }

    // Select RC48 as HSI48 for USB and RNG
    RCC->CCIPR |= RCC_CCIPR_HSI48SEL;

    // Synchronise HSI48 with 1kHz USB SoF
    __HAL_RCC_CRS_CLK_ENABLE();
    CRS->CR = 0x20 << CRS_CR_TRIM_Pos;
    CRS->CFGR = 2 << CRS_CFGR_SYNCSRC_Pos | 0x22 << CRS_CFGR_FELIM_Pos
        | __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000) << CRS_CFGR_RELOAD_Pos;
    #endif

    // Disable the Debug Module in low-power mode due to prevent
    // unexpected HardFault after __WFI().
    #if !defined(NDEBUG)
    DBGMCU->CR &= ~(DBGMCU_CR_DBG_SLEEP | DBGMCU_CR_DBG_STOP | DBGMCU_CR_DBG_STANDBY);
    #endif
}
#elif defined(STM32WB)

void SystemClock_Config(void) {
    while (LL_HSEM_1StepLock(HSEM, CFG_HW_RCC_SEMID)) {
    }

    // Enable the 32MHz external oscillator
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
    }

    // Prevent CPU2 from disabling CLK48.
    // This semaphore protected access to the CLK48 configuration.
    // CPU1 should hold this semaphore while the USB peripheral is in use.
    // See AN5289 and https://github.com/micropython/micropython/issues/6316.
    while (LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID)) {
    }

    // Use HSE and the PLL to get a 64MHz SYSCLK
    #define PLLM (HSE_VALUE / 8000000) // VCO input is 8MHz
    #define PLLN (24) // 24*8MHz = 192MHz
    #define PLLQ (4) // f_Q = 48MHz
    #define PLLR (3) // f_R = 64MHz
    RCC->PLLCFGR =
        (PLLR - 1) << RCC_PLLCFGR_PLLR_Pos | RCC_PLLCFGR_PLLREN
            | (PLLQ - 1) << RCC_PLLCFGR_PLLQ_Pos | RCC_PLLCFGR_PLLQEN
            | PLLN << RCC_PLLCFGR_PLLN_Pos
            | (PLLM - 1) << RCC_PLLCFGR_PLLM_Pos
            | 3 << RCC_PLLCFGR_PLLSRC_Pos;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
        // Wait for PLL to lock
    }
    const uint32_t sysclk_src = 3;

    // Set divider for HCLK2 to 2 so f_HCLK2 = 32MHz
    RCC->EXTCFGR = 8 << RCC_EXTCFGR_C2HPRE_Pos;

    // Set flash latency to 3 because SYSCLK > 54MHz
    FLASH->ACR |= 3 << FLASH_ACR_LATENCY_Pos;

    // Select SYSCLK source
    RCC->CFGR |= sysclk_src << RCC_CFGR_SW_Pos;
    while (((RCC->CFGR >> RCC_CFGR_SWS_Pos) & 0x3) != sysclk_src) {
        // Wait for SYSCLK source to change
    }

    // Select PLLQ as 48MHz source for USB and RNG
    RCC->CCIPR = 2 << RCC_CCIPR_CLK48SEL_Pos;

    SystemCoreClockUpdate();
    powerctrl_config_systick();

    // Release RCC semaphore
    LL_HSEM_ReleaseLock(HSEM, CFG_HW_RCC_SEMID, 0);
}

#elif defined(STM32WL)

#include "stm32wlxx_ll_utils.h"

void SystemClock_Config(void) {
    // Set flash latency
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_2);
    while (LL_FLASH_GetLatency() != LL_FLASH_LATENCY_2) {
    }

    LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);

    // Enable MSI
    LL_RCC_MSI_Enable();
    while (!LL_RCC_MSI_IsReady()) {
    }

    // Configure MSI
    LL_RCC_MSI_EnableRangeSelection();
    LL_RCC_MSI_SetRange(LL_RCC_MSIRANGE_11);
    LL_RCC_MSI_SetCalibTrimming(0);

    // Select SYSCLK source
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_MSI);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_MSI) {
    }

    // Set bus dividers
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAHB3Prescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);

    SystemCoreClockUpdate();
    powerctrl_config_systick();
}

#endif
