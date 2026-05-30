#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

// $[ACMP0]
// [ACMP0]$

// $[ACMP1]
// [ACMP1]$

// $[ADC0]
// [ADC0]$

// $[CMU]
// [CMU]$

// $[CSEN]
// [CSEN]$

// $[DBG]
// [DBG]$

// $[ETM]
// [ETM]$

// $[PTI]
// [PTI]$

// $[GPIO]
// [GPIO]$

// $[I2C0]
// [I2C0]$

// $[I2C1]
// [I2C1]$

// $[IDAC0]
// [IDAC0]$

// $[LESENSE]
// [LESENSE]$

// $[LETIMER0]
// [LETIMER0]$

// $[LEUART0]
// [LEUART0]$

// $[LFXO]
// [LFXO]$

// $[MODEM]
// [MODEM]$

// $[PCNT0]
// [PCNT0]$

// $[PCNT1]
// [PCNT1]$

// $[PCNT2]
// [PCNT2]$

// $[PRS.CH0]
// [PRS.CH0]$

// $[PRS.CH1]
// [PRS.CH1]$

// $[PRS.CH2]
// [PRS.CH2]$

// $[PRS.CH3]
// [PRS.CH3]$

// $[PRS.CH4]
// [PRS.CH4]$

// $[PRS.CH5]
// [PRS.CH5]$

// $[PRS.CH6]
// [PRS.CH6]$

// $[PRS.CH7]
// [PRS.CH7]$

// $[PRS.CH8]
// [PRS.CH8]$

// $[PRS.CH9]
// [PRS.CH9]$

// $[PRS.CH10]
// [PRS.CH10]$

// $[PRS.CH11]
// [PRS.CH11]$

// $[TIMER0]
// [TIMER0]$

// $[TIMER1]
// [TIMER1]$

// $[USART0]
// [USART0]$

// $[USART1]
// USART1 CLK on PC8
#ifndef USART1_CLK_PORT                         
#define USART1_CLK_PORT                          gpioPortC
#endif
#ifndef USART1_CLK_PIN                          
#define USART1_CLK_PIN                           8
#endif
#ifndef USART1_CLK_LOC                          
#define USART1_CLK_LOC                           11
#endif

// USART1 CS on PA4
#ifndef USART1_CS_PORT                          
#define USART1_CS_PORT                           gpioPortA
#endif
#ifndef USART1_CS_PIN                           
#define USART1_CS_PIN                            4
#endif
#ifndef USART1_CS_LOC                           
#define USART1_CS_LOC                            1
#endif

// USART1 RX on PC7
#ifndef USART1_RX_PORT                          
#define USART1_RX_PORT                           gpioPortC
#endif
#ifndef USART1_RX_PIN                           
#define USART1_RX_PIN                            7
#endif
#ifndef USART1_RX_LOC                           
#define USART1_RX_LOC                            11
#endif

// USART1 TX on PC6
#ifndef USART1_TX_PORT                          
#define USART1_TX_PORT                           gpioPortC
#endif
#ifndef USART1_TX_PIN                           
#define USART1_TX_PIN                            6
#endif
#ifndef USART1_TX_LOC                           
#define USART1_TX_LOC                            11
#endif

// [USART1]$

// $[USART2]
// [USART2]$

// $[USART3]
// [USART3]$

// $[VDAC0]
// [VDAC0]$

// $[WTIMER0]
// [WTIMER0]$

// $[WTIMER1]
// [WTIMER1]$

// $[CUSTOM_PIN_NAME]
#ifndef _PORT                                   
#define _PORT                                    gpioPortA
#endif
#ifndef _PIN                                    
#define _PIN                                     0
#endif

// [CUSTOM_PIN_NAME]$

#endif // PIN_CONFIG_H

