#ifndef __SGPIO_SLAVE_H__
#define __SGPIO_SLAVE_H__

/*
 * MS51 SGPIO RX-only pin map:
 *   SGPIO SCLK      -> MS51 P0.0 GPIO input, shared GPIO port ISR rising sampler
 *   SGPIO SDATA OUT -> MS51 P1.0 GPIO input, sampled by SCLK
 *   SGPIO SLOAD     -> MS51 P1.1 GPIO input, sampled by SCLK
 */
#define SGPIO_SLAVE_SCLK_PIN_MASK       (0x01U)
#define SGPIO_SLAVE_SDOUT_PIN_MASK      (0x01U)
#define SGPIO_SLAVE_SLOAD_PIN_MASK      (0x02U)

#define SGPIO_SLAVE_SCLK_IO             ((uint8_t)((P00 != 0U) ? 1U : 0U))
#define SGPIO_SLAVE_SDOUT_IO            ((uint8_t)((P10 != 0U) ? 1U : 0U))
#define SGPIO_SLAVE_SLOAD_IO            ((uint8_t)((P11 != 0U) ? 1U : 0U))

#define SGPIO_SLAVE_SCLK_INPUT_MODE     P00_INPUT_MODE
#define SGPIO_SLAVE_SDOUT_INPUT_MODE    P10_INPUT_MODE
#define SGPIO_SLAVE_SLOAD_INPUT_MODE    P11_INPUT_MODE

#define SGPIO_SLAVE_SELECT_INT_PORT     clr_PICON_PIPS1; clr_PICON_PIPS0
#define SGPIO_SLAVE_ENABLE_SCLK_TRIG    ENABLE_BIT0_RISINGEDGE_TRIG
#define SGPIO_SLAVE_CLEAR_SCLK_INT_FLAG CLEAR_PIN_INTERRUPT_PIT0_FLAG
#define SGPIO_SLAVE_SCLK_INT_PENDING    ((PIF & SGPIO_SLAVE_SCLK_PIN_MASK) != 0U)

#define SGPIO_SLAVE_SLOAD_PIN_NAME      "P1.1"
#define SGPIO_SLAVE_SDOUT_PIN_NAME      "P1.0"
#define SGPIO_SLAVE_SCLK_PIN_NAME       "P0.0"

#define SGPIO_SLAVE_MAX_SLOTS           (16U)
#define SGPIO_SLAVE_RX_MAX_BYTES        (8U)

void SGPIO_Init(void);
void SGPIO_Process(void);
void SGPIO_OnClockRisingSampledIrq(uint8_t sload_value, uint8_t sdata_value);
void SGPIO_PinInterruptHandler(void);

#endif
