/*_____ I N C L U D E S ____________________________________________________*/
#include "numicro_8051.h"

#include "misc_config.h"
#include "sgpio_slave.h"

/*_____ D E C L A R A T I O N S ____________________________________________*/

#define TIMER_DIV12_1ms_FOSC_240000  			        (65536-2000)
#define TH0_INIT        						        (HIBYTE(TIMER_DIV12_1ms_FOSC_240000)) 
#define TL0_INIT        						        (LOBYTE(TIMER_DIV12_1ms_FOSC_240000))


//UART 0
bit BIT_TMP;
bit BIT_UART;
bit uart0_receive_flag=0;
unsigned char uart0_receive_data;

volatile struct flag_32bit flag_PROJ_CTL;
#define FLAG_PROJ_TIMER_PERIOD_1000MS                 	(flag_PROJ_CTL.bit0)
#define FLAG_PROJ_REVERSE1                   			(flag_PROJ_CTL.bit1)
#define FLAG_PROJ_REVERSE2                 				(flag_PROJ_CTL.bit2)
#define FLAG_PROJ_REVERSE3                              (flag_PROJ_CTL.bit3)
#define FLAG_PROJ_REVERSE4                              (flag_PROJ_CTL.bit4)
#define FLAG_PROJ_REVERSE5                              (flag_PROJ_CTL.bit5)
#define FLAG_PROJ_REVERSE6                              (flag_PROJ_CTL.bit6)
#define FLAG_PROJ_REVERSE7                              (flag_PROJ_CTL.bit7)


/*_____ D E F I N I T I O N S ______________________________________________*/
volatile uint32_t counter_tick = 0;

/*_____ M A C R O S ________________________________________________________*/
#define SYS_CLOCK 								        (24000000ul)


/*_____ F U N C T I O N S __________________________________________________*/

static uint32_t get_tick(void)
{
	return (counter_tick);
}

unsigned long get_systick(void)
{
	return ((unsigned long)counter_tick);
}

static void set_tick(uint32_t t)
{
	counter_tick = t;
}

static void tick_counter(void)
{
	counter_tick++;
}

void delay(uint16_t dly)
{
/*
	delay(100) : 14.84 us
	delay(200) : 29.37 us
	delay(300) : 43.97 us
	delay(400) : 58.5 us	
	delay(500) : 73.13 us	
	
	delay(1500) : 0.218 ms (218 us)
	delay(2000) : 0.291 ms (291 us)	
*/

	while( dly--);
}


void loop(void)
{
	// static uint16_t LOG = 0;	
	SGPIO_Process();

	if (FLAG_PROJ_TIMER_PERIOD_1000MS)
	{
		FLAG_PROJ_TIMER_PERIOD_1000MS = 0;	
		// printf("LOG : %4u\r\n",LOG++);
		P12 ^= 1;		
	}
}

void GPIO_Init(void)
{
	P12 = 0;
	// P17 = 0;
	// P30 = 0;
	
	P12_PUSHPULL_MODE;		
	// P17_QUASI_MODE;		
	// P30_PUSHPULL_MODE;	
}

void Timer0_IRQHandler(void)
{

	tick_counter();

	if ((get_tick() % 1000) == 0)
	{
		FLAG_PROJ_TIMER_PERIOD_1000MS = 1;

	}

	if ((get_tick() % 50) == 0)
	{

	}		
}

void Timer0_ISR(void) interrupt 1        // Vector @  0x0B
{
    _push_(SFRS);	
	
    clr_TCON_TF0;
	TH0 = TH0_INIT;
	TL0 = TL0_INIT;	
	
	Timer0_IRQHandler();

    _pop_(SFRS);	
}

void TIMER0_Init(void)
{
	/*
		formula : 16bit 
		(0xFFFF+1 - target)  / (24MHz/psc) = time base 
	*/	
	
	ENABLE_TIMER0_MODE1;	// mode 0 : 13 bit , mode 1 : 16 bit
    TIMER0_FSYS_DIV12;

	TH0 = TH0_INIT;
	TL0 = TL0_INIT;
	clr_TCON_TF0;

    set_TCON_TR0;                                  //Timer0 run
    ENABLE_TIMER0_INTERRUPT;                       //enable Timer0 interrupt
    ENABLE_GLOBAL_INTERRUPT;                       //enable interrupts
  
}

void Serial_ISR (void) interrupt 4 
{
    _push_(SFRS);

    if (RI)
    {   
      uart0_receive_flag = 1;
      uart0_receive_data = SBUF;
      clr_SCON_RI;                                         // Clear RI (Receive Interrupt).
    }
    if  (TI)
    {
      if(!BIT_UART)
      {
          TI = 0;
      }
    }

    _pop_(SFRS);	
}

void Pin_INT_ISR(void) interrupt 7        // Vector @  0x3B
{
    _push_(SFRS);

	SGPIO_PinInterruptHandler();

    _pop_(SFRS);
}

void UART0_Init(void)
{
	#if 1
	const unsigned long u32Baudrate = 115200;
	P06_QUASI_MODE;    //Setting UART pin as Quasi mode for transmit
	
	SCON = 0x50;          //UART0 Mode1,REN=1,TI=1
	set_PCON_SMOD;        //UART0 Double Rate Enable
	T3CON &= 0xF8;        //T3PS2=0,T3PS1=0,T3PS0=0(Prescale=1)
	set_T3CON_BRCK;        //UART0 baud rate clock source = Timer3

	RH3    = HIBYTE(65536 - (SYS_CLOCK/16/u32Baudrate));  
	RL3    = LOBYTE(65536 - (SYS_CLOCK/16/u32Baudrate));  
	
	set_T3CON_TR3;         //Trigger Timer3
	
	/*
		set UART priority
		IPH / EIPH / EIPH1 	, IP / EIP / EIP2 
		0  						0  					Level 0 (lowest) 
		0  						1  					Level 1 
		1  						0  					Level 2 
		1  						1  					Level 3 (highest) 

		SET_INT_UART0_LEVEL0;	//clr_IP_PS; clr_IPH_PSH; //0
		SET_INT_UART0_LEVEL1;	//clr_IP_PS; set_IPH_PSH; //1
		SET_INT_UART0_LEVEL2;	//set_IP_PS; clr_IPH_PSH; //2
		SET_INT_UART0_LEVEL3;	//set_IP_PS; set_IPH_PSH; //3
	*/
	
	ENABLE_UART0_INTERRUPT;
	ENABLE_GLOBAL_INTERRUPT;

	set_SCON_TI;
	BIT_UART=1;
	#else	
    UART_Open(SYS_CLOCK,UART0_Timer3,115200);
    ENABLE_UART0_PRINTF; 
	#endif
}


void MODIFY_HIRC_24(void)
{
	unsigned char u8HIRCSEL = HIRC_24;
    unsigned char data hircmap0,hircmap1;
//    unsigned int trimvalue16bit;
    /* Check if power on reset, modify HIRC */
    set_CHPCON_IAPEN;
    SFRS = 0 ;
	#if 1
    IAPAL = 0x38;
	#else
    switch (u8HIRCSEL)
    {
      case HIRC_24:
        IAPAL = 0x38;
      break;
      case HIRC_16:
        IAPAL = 0x30;
      break;
      case HIRC_166:
        IAPAL = 0x30;
      break;
    }
	#endif
	
    IAPAH = 0x00;
    IAPCN = READ_UID;
    set_IAPTRG_IAPGO;
    hircmap0 = IAPFD;
    IAPAL++;
    set_IAPTRG_IAPGO;
    hircmap1 = IAPFD;
    // clr_CHPCON_IAPEN;

	#if 0
    switch (u8HIRCSEL)
    {
		case HIRC_166:
		trimvalue16bit = ((hircmap0 << 1) + (hircmap1 & 0x01));
		trimvalue16bit = trimvalue16bit - 15;
		hircmap1 = trimvalue16bit & 0x01;
		hircmap0 = trimvalue16bit >> 1;

		break;
		default: break;
    }
	#endif
	
    TA = 0xAA;
    TA = 0x55;
    RCTRIM0 = hircmap0;
    TA = 0xAA;
    TA = 0x55;
    RCTRIM1 = hircmap1;
    clr_CHPCON_IAPEN;
    // PCON &= CLR_BIT4;
}


void SYS_Init(void)
{
    MODIFY_HIRC_24();

    // ALL_GPIO_QUASI_MODE;
    ENABLE_GLOBAL_INTERRUPT;                // global enable bit	
}

void main (void) 
{
    SYS_Init();

	UART0_Init();
	GPIO_Init();
	TIMER0_Init();

	/*
	 * SGPIO RX-only GPIO rising-edge sampler:
	 *   SGPIO SCLK      -> MS51 P0.0 GPIO input, pin interrupt rising sampler
	 *   SGPIO SDATA OUT -> MS51 P1.0 GPIO input, sampled by SCLK
	 *   SGPIO SLOAD     -> MS51 P1.1 GPIO input, sampled by SCLK
	 */
	SGPIO_Init();


		
    while(1)
    {
		loop();
			
    }
}



