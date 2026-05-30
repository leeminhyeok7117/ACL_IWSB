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
// DBG SWV on PF2
#ifndef DBG_SWV_PORT                            
#define DBG_SWV_PORT                             gpioPortF
#endif
#ifndef DBG_SWV_PIN                             
#define DBG_SWV_PIN                              2
#endif
#ifndef DBG_SWV_LOC                             
#define DBG_SWV_LOC                              0
#endif

// [DBG]$

// $[ETM]
// [ETM]$

// $[PTI]
// PTI DFRAME on PB13
#ifndef PTI_DFRAME_PORT                         
#define PTI_DFRAME_PORT                          gpioPortB
#endif
#ifndef PTI_DFRAME_PIN                          
#define PTI_DFRAME_PIN                           13
#endif
#ifndef PTI_DFRAME_LOC                          
#define PTI_DFRAME_LOC                           6
#endif

// PTI DOUT on PB12
#ifndef PTI_DOUT_PORT                           
#define PTI_DOUT_PORT                            gpioPortB
#endif
#ifndef PTI_DOUT_PIN                            
#define PTI_DOUT_PIN                             12
#endif
#ifndef PTI_DOUT_LOC                            
#define PTI_DOUT_LOC                             6
#endif

// [PTI]$

// $[GPIO]
// [GPIO]$

// $[I2C0]
// I2C0 SCL on PC10
#ifndef I2C0_SCL_PORT                           
#define I2C0_SCL_PORT                            gpioPortC
#endif
#ifndef I2C0_SCL_PIN                            
#define I2C0_SCL_PIN                             10
#endif
#ifndef I2C0_SCL_LOC                            
#define I2C0_SCL_LOC                             14
#endif

// I2C0 SDA on PC11
#ifndef I2C0_SDA_PORT                           
#define I2C0_SDA_PORT                            gpioPortC
#endif
#ifndef I2C0_SDA_PIN                            
#define I2C0_SDA_PIN                             11
#endif
#ifndef I2C0_SDA_LOC                            
#define I2C0_SDA_LOC                             16
#endif

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
// USART0 CTS on PA2
#ifndef USART0_CTS_PORT                         
#define USART0_CTS_PORT                          gpioPortA
#endif
#ifndef USART0_CTS_PIN                          
#define USART0_CTS_PIN                           2
#endif
#ifndef USART0_CTS_LOC                          
#define USART0_CTS_LOC                           30
#endif

// USART0 RTS on PA3
#ifndef USART0_RTS_PORT                         
#define USART0_RTS_PORT                          gpioPortA
#endif
#ifndef USART0_RTS_PIN                          
#define USART0_RTS_PIN                           3
#endif
#ifndef USART0_RTS_LOC                          
#define USART0_RTS_LOC                           30
#endif

// USART0 RX on PA1
#ifndef USART0_RX_PORT                          
#define USART0_RX_PORT                           gpioPortA
#endif
#ifndef USART0_RX_PIN                           
#define USART0_RX_PIN                            1
#endif
#ifndef USART0_RX_LOC                           
#define USART0_RX_LOC                            0
#endif

// USART0 TX on PA0
#ifndef USART0_TX_PORT                          
#define USART0_TX_PORT                           gpioPortA
#endif
#ifndef USART0_TX_PIN                           
#define USART0_TX_PIN                            0
#endif
#ifndef USART0_TX_LOC                           
#define USART0_TX_LOC                            0
#endif

// [USART0]$

// $[USART1]
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

