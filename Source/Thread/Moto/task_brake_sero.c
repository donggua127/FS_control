#include "task_brake_servo.h"
#include "task_moto.h"
#include "uartns550.h"
/*SYSBIOS includes*/
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <xdc/runtime/System.h>
#include <xdc/runtime/Error.h>
#include <ti/sysbios/BIOS.h>
#include "Message/Message.h"
#include "common.h"
#include "fpga_ttl.h"
#include "canModule.h"

extern uint16_t getRPM(void);

#define BRAKE_MBOX_DEPTH (16)
#define CAN_DEV_BRAKE (4)

extern uint16_t MotoGetRPM(void);


/********************************************************************************/
/*          静态全局变量                                                              */
/********************************************************************************/
static Semaphore_Handle txReadySem;
static uint8_t uBrake,uLastBrake;	//刹车信号
static int8_t  deltaBrake;
static int16_t sDeltaRPM,sBrake;
static uint8_t servoStep=0;			//标记伺服位置
static int32_t pulseCount;			//刹车位置与伺服当前位置差
static int16_t pulseE4;
static int16_t pulseE0;


static uint8_t railState;
static uint8_t keyState;


static Semaphore_Handle sem_txData;
static Semaphore_Handle sem_rxData;
static Semaphore_Handle sem_modbus;
static Mailbox_Handle recvMbox;
static uartDataObj_t brakeUartDataObj;
static uartDataObj_t recvUartDataObj;

static uint8_t ackStatus = MODBUS_ACK_OK;

static uint8_t changeRail = 0;

static brake_ctrl_t m_brakeCtrl = {
		.Brake = 0,
		.BrakeReady = 1
};
static rail_ctrl_t m_railCtrl = {
		.RailState = LEFTRAIL
};

static void ServorUartIntrHandler(void *callBackRef, u32 event, unsigned int eventData)
{
	uint8_t Errors;
	uint16_t UartDeviceNum = *((u16 *)callBackRef);
    uint8_t *NextBuffer;
    
	/*
	 * All of the data has been sent.
	 */
	if (event == XUN_EVENT_SENT_DATA) {
        Semaphore_post(sem_txData);
	}



	if (event == XUN_EVENT_RECV_TIMEOUT || event == XUN_EVENT_RECV_DATA) {

        brakeUartDataObj.length = eventData;
        Mailbox_post(recvMbox, (Ptr *)&brakeUartDataObj, BIOS_NO_WAIT);
        UartNs550Recv(UartDeviceNum, &brakeUartDataObj.buffer, UART_REC_BUFFER_SIZE);
	}


	/*
	 * Data was received with an error, keep the data but determine
	 * what kind of errors occurred.
	 */
	if (event == XUN_EVENT_RECV_ERROR) {

		Errors = UartNs550GetLastErrors(UartDeviceNum);
	}
}



static void ServoInitSem(void)
{

	Semaphore_Params semParams;
    Mailbox_Params mboxParams; 
    /* 初始化接收邮箱 */
    Mailbox_Params_init(&mboxParams);
    recvMbox = Mailbox_create (sizeof (uartDataObj_t),BRAKE_MBOX_DEPTH, &mboxParams, NULL);
    
	Semaphore_Params_init(&semParams);
    semParams.mode = Semaphore_Mode_BINARY;
    sem_rxData = Semaphore_create(0, &semParams, NULL);
    sem_txData = Semaphore_create(0, &semParams, NULL);
    sem_modbus = Semaphore_create(1, &semParams, NULL);
}
 




//u8 *data;//数据起始地址，用于计算 CRC 值
//u8 length; //数据长度
//返回 unsigned integer 类型的 CRC 值。
static uint16_t ServoCrcCheck(uint8_t *data, uint8_t length)
{
	uint8_t j;
	uint16_t crc_reg = 0xFFFF;
	while (length--)
	{
		crc_reg ^= *data++;
		for (j = 0; j < 8; j++)
		{
			if (crc_reg & 0x01)
			{
				crc_reg = (crc_reg >> 1) ^ 0xA001;
			}
			else
			{
				crc_reg = crc_reg >> 1;
			}
		}
	}
	return crc_reg;
}


static uint8_t ServoModbusWriteReg(uint8_t id,uint16_t addr,uint16_t data)
{
	uint16_t udelay;
    modbusCmd_t sendData;

    Semaphore_pend(sem_modbus,BIOS_WAIT_FOREVER);   
    
    //for(udelay = 3000;udelay>0;udelay--);           //增加发送间隔，保证控制器能识别

    sendData.id = id;
    sendData.cmd = 0x06;
    sendData.addrH = addr >> 8;
    sendData.addrL = addr & 0x00ff;
    sendData.dataH = data >> 8;
    sendData.dataL = data & 0x00ff;
    sendData.crc = ServoCrcCheck((uint8_t *)&sendData,6);
    
    /*使能RS485发送*/
    UartNs550RS485TxEnable(SERVOR_MOTOR_UART);

    /*清除接收信用量*/
    Semaphore_pend(sem_rxData,BIOS_NO_WAIT);
    /*清除接收数据计数*/
    //recvCnt = 0;

    /*发送数据*/
    UartNs550Send(SERVOR_MOTOR_UART,(uint8_t *)&sendData,8);

    
    /*等待发送完成*/
    if(FALSE == Semaphore_pend(sem_txData,10))
    {
        /*
         * 10ms发送超时
         */
        ackStatus = MODBUS_ACK_SENDERR;
        UartNs550RS485TxDisable(SERVOR_MOTOR_UART);
        Semaphore_post(sem_modbus);
        return ackStatus;
    }

    /*关闭RS485发送*/
    for(udelay = 3000;udelay>0;udelay--);           //延时等待最后一个串口数据被驱动到总线上
    UartNs550RS485TxDisable(SERVOR_MOTOR_UART);

    /*等待电机响应*/
    if(FALSE == Semaphore_pend(sem_rxData,10))   //丢弃MAX3160发送时，回传的数据
    {
        /*
         * 环回数据超时
         *
         */
        ackStatus = MODBUS_ACK_LOOPERR;
        Semaphore_post(sem_modbus);
        return ackStatus;
    }

    
    if(FALSE  == Semaphore_pend(sem_rxData,10))    //电机ACK超时时间：100ms
    {
    	ackStatus = MODBUS_ACK_TIMEOUT;
    }


    Semaphore_post(sem_modbus);
    return ackStatus;
}


static uint8_t ServoModbusReadReg(uint8_t id,uint16_t addr,uint16_t *data)
{
	uint16_t udelay;
    modbusCmd_t sendData;

    Semaphore_pend(sem_modbus,BIOS_WAIT_FOREVER);   
    
    //for(udelay = 3000;udelay>0;udelay--);           //增加发送间隔，保证控制器能识别

    sendData.id = id;
    sendData.cmd = 0x03;
    sendData.addrH = addr >> 8;
    sendData.addrL = addr & 0x00ff;
    sendData.dataH = 0;
    sendData.dataL = 1;
    sendData.crc = ServoCrcCheck((uint8_t *)&sendData,6);
    
    /*使能RS485发送*/
    UartNs550RS485TxEnable(SERVOR_MOTOR_UART);

    /*清除接收信用量*/
    Semaphore_pend(sem_rxData,BIOS_NO_WAIT);
    /*清除接收数据计数*/
    //recvCnt = 0;

    /*发送数据*/
    UartNs550Send(SERVOR_MOTOR_UART,(uint8_t *)&sendData,8);

    
    /*等待发送完成*/
    if(FALSE == Semaphore_pend(sem_txData,10))
    {
        /*
         * 10ms发送超时
         */
        ackStatus = MODBUS_ACK_SENDERR;
        UartNs550RS485TxDisable(SERVOR_MOTOR_UART);
        Semaphore_post(sem_modbus);
        return ackStatus;
    }

    /*关闭RS485发送*/
    for(udelay = 3000;udelay>0;udelay--);           //延时等待最后一个串口数据被驱动到总线上
    UartNs550RS485TxDisable(SERVOR_MOTOR_UART);

    /*等待电机响应*/
    if(FALSE == Semaphore_pend(sem_rxData,10))   //丢弃MAX3160发送时，回传的数据
    {
        /*
         * 环回数据超时
         *
         */
        ackStatus = MODBUS_ACK_LOOPERR;
        Semaphore_post(sem_modbus);
        return ackStatus;
    }

    
    if(FALSE  == Semaphore_pend(sem_rxData,10))    //电机ACK超时时间：10ms
    	ackStatus = MODBUS_ACK_TIMEOUT;
    else
        *data = (uint16_t)(recvUartDataObj.buffer[3] << 8) + recvUartDataObj.buffer[4];

    Semaphore_post(sem_modbus);
    
    return ackStatus;
}


static void ServoRecvDataTask(void)
{
    
    uint16_t calcCrc;
    uint16_t recvCrc;
    while(1)
    {
        Mailbox_pend(recvMbox,(Ptr*) &recvUartDataObj, BIOS_WAIT_FOREVER);
        
        if(recvUartDataObj.length == 8 || recvUartDataObj.length == 6 || recvUartDataObj.length == 5 || recvUartDataObj.length == 7)
        {
            calcCrc = ServoCrcCheck((uint8_t *)recvUartDataObj.buffer,recvUartDataObj.length - 2);
            recvCrc = ((uint16_t)recvUartDataObj.buffer[recvUartDataObj.length-2]) + (((uint16_t)recvUartDataObj.buffer[recvUartDataObj.length-1])<<8);
            if(recvCrc == calcCrc)
            {
                if(recvUartDataObj.length == 8 || recvUartDataObj.length == 7)
                    ackStatus = MODBUS_ACK_OK;
                else
                    ackStatus = MODBUS_ACK_NOTOK;
            }
            else
                ackStatus = MODBUS_ACK_CRC_ERR;
        }
        else
        {
            ackStatus = MODBUS_ACK_FRAME_ERR;
        }

        Semaphore_post(sem_rxData);
    }
}

/*
 * 刹车模式定义(预编译)
 * 0:伺服位置模式
 * 1:伺服转矩模式
 * 2:直流电机模式
 * 3:直流电机(CAN)
 */
#define SERVO_MODE (3)

#if SERVO_MODE == 0
void ServoBrakeTask(void *param)
{
	uint8_t state;
    p_msg_t sendmsg;
    uint8_t brakeTimeoutCnt = 0;
    uint16_t recvReg;
    uint8_t motoEnableFlag = 0;

    /*Pn070 Son使能驱动器*/
    ServoModbusWriteReg(0x01,0x0046,0x7FFE);

    /*Pn071 内部位置0,并取消触发*/
    ServoModbusWriteReg(0x01,0x0047,0x7FFF);
	
	/*Pn120 内部位置0(万)*/
    ServoModbusWriteReg(0x01,0x0078,0x0000);

    //Pn121	内部位置0（个）
    ServoModbusWriteReg(0x01,0x0079,0x0000);
	
	while (1)
	{	
		Task_sleep(BRAKETIME);
        
        
        if(brakeTimeoutCnt > BRAKE_TIMEOUT)
        {
            /*
            *TODO:添加通信超时，发送急停消息，进入急停模式，并设置ErrorCode
            */
            sendmsg = Message_getEmpty();
            sendmsg->type = error;
            sendmsg->data[0] = ERROR_BRAKE_TIMEOUT;
            Message_post(sendmsg);
            brakeTimeoutCnt = 0;
            servoStep = 0;    
            BrakeSetReady(0);
            continue;
        }


        if(BrakeGetReady() == 0)
        	continue;
        

		uBrake = BrakeGetBrake();
		/*	总行程45000个脉冲
			每步450脉冲
		*/
		if(servoStep!=uBrake)
		{
			pulseCount=BRAKE_STEP_PULSE*(uBrake-servoStep);
			pulseE4=pulseCount/10000;				//万位
			pulseE0=pulseCount-10000*pulseE4;		//个位
			while(1)
			{
				state = ServoModbusReadReg(0x01,0x0182,&recvReg);
				if(state == MODBUS_ACK_OK)
				{
					if((recvReg & 0x03) != 0)
					{
							/*
							*TODO:添加通信超时，发送急停消息，进入急停模式，并设置ErrorCode
							*/
							sendmsg = Message_getEmpty();
							sendmsg->type = error;
							sendmsg->data[0] = ERROR_BRAKE_ERROR;
							Message_post(sendmsg);
							servoStep = 0;
							BrakeSetReady(0);
							break;
					}
				}
				else if(state == MODBUS_ACK_TIMEOUT)
				{
					brakeTimeoutCnt ++;
					break;
				}
				else
					brakeTimeoutCnt = 0;


				state = ServoModbusReadReg(0x01,0x0046,&recvReg);
				if(state == MODBUS_ACK_OK)
				{
					if(recvReg == 0x7fff)		//未使能
						motoEnableFlag = 1;
					else
						motoEnableFlag = 0;

					brakeTimeoutCnt = 0;
				}
				else
				{
					brakeTimeoutCnt ++;
					break;
				}

				if(motoEnableFlag == 1)
				{
					state = ServoModbusWriteReg(0x01,0x0046,0x7FFE);	//使能

					if(state != MODBUS_ACK_OK)
					{
						brakeTimeoutCnt ++;
						break;
					}
					else
						brakeTimeoutCnt = 0;

					Task_sleep(100);
				}

				Task_sleep(SLEEPTIME);
                state = ServoModbusWriteReg(0x01,0x0047,0x7FFF);
				
				if(state != MODBUS_ACK_OK)
				{
                    brakeTimeoutCnt ++;
                    break;
				}
                else
                    brakeTimeoutCnt = 0;

				Task_sleep(SLEEPTIME);
                state = ServoModbusWriteReg(0x01,0x0078,pulseE4);
				
				if(state != MODBUS_ACK_OK)
				{
                    brakeTimeoutCnt ++;
                    break;
				}
                else
                    brakeTimeoutCnt = 0;

				Task_sleep(SLEEPTIME);
                state = ServoModbusWriteReg(0x01,0x0079,pulseE0);
				
				if(state != MODBUS_ACK_OK)
				{
                    brakeTimeoutCnt ++;
                    break;
				}
                else
                    brakeTimeoutCnt = 0;
			
				Task_sleep(SLEEPTIME);
				state = ServoModbusWriteReg(0x01,0x0047,0x7BFF);
                
                if(state != MODBUS_ACK_TIMEOUT)   //只要不是超时，都认为是电机响应了
                {
				    servoStep=uBrake;		//标记伺服位置
				    brakeTimeoutCnt = 0;
				    break;
                }
				else
				{
                    brakeTimeoutCnt ++;
                    break;
				}
				
			}/*end of while(1)*/
		}/* end of if(servo_step!=uBrake)*/
		else
		{


		}

	}
}

#elif SERVO_MODE == 1
void ServoBrakeTask(void *param)
{
    uint8_t state;
    p_msg_t sendmsg;
    uint8_t brakeTimeoutCnt = 0;
    uint16_t recvReg;
    int16_t ubrakeOld = -6;
    int16_t ubrake;

    /*设置转矩来源：内部转矩*/
    ServoModbusWriteReg(0x01,0x00cc,1);

    while (1)
    {
        Task_sleep(BRAKETIME);
        ubrake = (int16_t)BrakeGetBrake() - 6;

        if(brakeTimeoutCnt > BRAKE_TIMEOUT)
        {
            /*
            *TODO:添加通信超时，发送急停消息，进入急停模式，并设置ErrorCode
            */
            sendmsg = Message_getEmpty();
            sendmsg->type = error;
            sendmsg->data[0] = ERROR_BRAKE_TIMEOUT;
            Message_post(sendmsg);
            brakeTimeoutCnt = 0;
            ubrakeOld = -6;
            BrakeSetReady(0);
            continue;
        }


        if(BrakeGetReady() == 0)
            continue;

        /*获取刹车驱动器故障代码*/
        state = ServoModbusReadReg(0x01,0x0182,&recvReg);
        if(state == MODBUS_ACK_OK)
        {
            if((recvReg & 0x03) != 0)
            {
                    /*
                    *TODO:刹车故障，上报错误
                    */
                    sendmsg = Message_getEmpty();
                    sendmsg->type = error;
                    sendmsg->data[0] = ERROR_BRAKE_ERROR;
                    Message_post(sendmsg);
                    ubrakeOld = -6;
                    BrakeSetReady(0);
                    break;
            }
            brakeTimeoutCnt = 0;
        }
        else
        {
            brakeTimeoutCnt ++;
        }

        if(ubrake != ubrakeOld)
        {
            while(1)
            {


                /*设置转矩大小0~100*/
                Task_sleep(SLEEPTIME);
                if(ubrake < 100)
                    state = ServoModbusWriteReg(0x01,0x00c8,ubrake);
                else
                    state = ServoModbusWriteReg(0x01,0x00c8,100);

                if(state != MODBUS_ACK_OK)
                {
                    brakeTimeoutCnt ++;
                    break;
                }
                else
                    brakeTimeoutCnt = 0;

                Task_sleep(SLEEPTIME);
    #if 0
                if(ubrake != 0)
                {
                    state = ServoModbusWriteReg(0x01,0x0046,0x7FB2);    //使能
                }
                else
                {
                    state = ServoModbusWriteReg(0x01,0x0046,0x7FB3);    //失能
                }
    #endif
                state = ServoModbusWriteReg(0x01,0x0046,0x7FB2);    //使能
                if(state != MODBUS_ACK_TIMEOUT)   //只要不是超时，都认为是电机响应了
                {
                    if(state == MODBUS_ACK_OK)
                        brakeTimeoutCnt = 0;

                    ubrakeOld = ubrake;
                    break;
                }
                else
                {
                    brakeTimeoutCnt ++;
                    break;
                }

            }/*end of while(1)*/
        }/*if(ubrake != ubrakeOld)*/


    }
}
#elif SERVO_MODE == 2
#define PWM_MAIN_CLK (80000000)
#define PWM_DUTY_BASE (255)
#define FPGA_PWM_FTW (SOC_EMIFA_CS2_ADDR + (0x30<<1))
#define FPGA_PWM_DUTY (SOC_EMIFA_CS2_ADDR + (0x34<<1))
#define FPGA_PWM_LOAD (SOC_EMIFA_CS2_ADDR + (0x38<<1))
#define FPGA_PWM_DIR (SOC_EMIFA_CS2_ADDR + (0x0A<<1))

static void ServoPWMSet(uint32_t freq,uint16_t duty)
{
    uint32_t ftw;
    uint32_t dutyCnt;
    ftw = PWM_MAIN_CLK/freq;

    //dutyCnt = (duty*ftw/PWM_DUTY_BASE);
    /*限定范围为0~20%*/
    dutyCnt = (duty*ftw/PWM_DUTY_BASE/5);

    EMIFAWriteWord(FPGA_PWM_FTW, 0, ftw & 0xff);
    EMIFAWriteWord(FPGA_PWM_FTW, 1, (ftw >> 8) & 0xff);
    EMIFAWriteWord(FPGA_PWM_FTW, 2, (ftw >> 16) & 0xff);
    EMIFAWriteWord(FPGA_PWM_FTW, 3, (ftw >> 24) & 0xff);
    /*
     * 设置占空比
     */
    EMIFAWriteWord(FPGA_PWM_DUTY, 0, dutyCnt & 0xff);
    EMIFAWriteWord(FPGA_PWM_DUTY, 1, (dutyCnt >> 8) & 0xff);
    EMIFAWriteWord(FPGA_PWM_DUTY, 2, (dutyCnt >> 16) & 0xff);
    EMIFAWriteWord(FPGA_PWM_DUTY, 3, (dutyCnt >> 24) & 0xff);
    /*
     * 载入设置数据
     */
    EMIFAWriteWord(FPGA_PWM_LOAD, 0, 1);
    EMIFAWriteWord(FPGA_PWM_LOAD, 0, 0);
}

void ServoBrakeTask(void *param)
{
    int16_t ubrake;

    while (1)
    {
        Task_sleep(BRAKETIME);
        ubrake = (int16_t)BrakeGetBrake();
        if(ubrake > 0)
        {
            /*正转*/
            EMIFAWriteWord(FPGA_PWM_DIR, 0, 0x40);
            ServoPWMSet(1000,ubrake);
        }
        else
        {
            /*反转*/
            EMIFAWriteWord(FPGA_PWM_DIR, 0, 0x00);
            ServoPWMSet(1000,20);
        }
    }
}
#elif SERVO_MODE == 3

#define BRAKE_MAX (255)
#if CAR_VERSION == 21
#define BRAKE_SLOTS (1000)
#define REVERSE_FORCE (100)

/*
 * 2.3的刹车减速机是反向的
 */
#elif CAR_VERSION == 23
#define BRAKE_SLOTS (-1000)
#define REVERSE_FORCE (-100)

#endif

static void BrakeCanIntrHandler(int32_t devsNum,int32_t event)
{
    canDataObj_t rxData;

	if (event == 1)         /* 收到一帧数据 */
    {
        CanRead(devsNum, &rxData);
//        Mailbox_post(rxDataMbox, (Ptr *)&rxData, BIOS_NO_WAIT);
	}
    else if (event == 2)    /* 一帧数据发送完成 */
    {
        /* 发送中断 */
        Semaphore_post(txReadySem);
    }

}

void ServoBrakeTask(void *param)
{
	canDataObj_t canData;
	int16_t brakeforce;
	Semaphore_Params semParams;

	/*初始化can帧*/
	memset(&canData, 0, sizeof(canData));
	canData.ID = 1;
	canData.SendType = 0;
	canData.RemoteFlag = 0;
	canData.ExternFlag = 0;
	canData.DataLen = 8;


    /* 初始化发送信用量 */
	Semaphore_Params_init(&semParams);
	semParams.mode = Semaphore_Mode_COUNTING;
    txReadySem = Semaphore_create(1, &semParams, NULL);

    /*初始化CAN设备*/
    CanOpen(CAN_DEV_BRAKE, BrakeCanIntrHandler, CAN_DEV_BRAKE);

    while(1)
    {
    	Task_sleep(BRAKETIME);

    	Semaphore_pend(txReadySem, BIOS_WAIT_FOREVER);

    	/*计算力矩*/
    	brakeforce =  -BrakeGetBrake() * BRAKE_SLOTS/BRAKE_MAX;

#if CAR_VERSION == 21
    	if(brakeforce>=0)
#elif CAR_VERSION == 23
    	if(brakeforce<=0)
#endif
    	{
    		brakeforce = REVERSE_FORCE;
    	}
    	canData.Data[5] = brakeforce & 0xFF;
    	canData.Data[6] = (brakeforce>>8) & 0xFF;
    	canData.Data[7] = 2;

    	CanWrite(CAN_DEV_BRAKE, &canData);
    }
}

#endif
void RailChangeStart()
{
	changeRail = 1;
}


static void ServoChangeRailTask(void)
{
	uint8_t state;
    uint16_t recvReg;
    p_msg_t sendmsg;
	static uint8_t change_en=1;
	uint16_t changerail_timeout_cnt = 0;


	uint8_t regv;
	/* 获取轨道状态 */
	regv = TTLRead();

	if((0x03 & regv) == LEFTRAIL)
	    RailSetRailState(LEFTRAIL);
	else if((0x03 & regv) == RIGHTRAIL)
	    RailSetRailState(RIGHTRAIL);
	else
	{
		/*
		*TODO:添加通信超时，发送急停消息，进入急停模式，并设置ErrorCode
		*/
		sendmsg = Message_getEmpty();
		sendmsg->type = error;
		sendmsg->data[0] = ERROR_CHANGERAIL_TIMEOUT;
		Message_post(sendmsg);
	}


	while(1)
	{
		Task_sleep(50);

		/* 更新轨道状态 */
		regv = TTLRead();
		RailSetRailState(regv & 0x03);


		if(1 == changeRail)
		{
			changeRail = 0;

			/*
			 * 1.设置电机方向
			 * 2.使能电机
			 * 3.等待超时
			 * 4.判断电机是否到位
			 */
			if(RailGetRailState() == LEFTRAIL)
				TTLWriteBit(RAIL_DIRECT,0);
			else
				TTLWriteBit(RAIL_DIRECT,1);

			Task_sleep(10);

			TTLWriteBit(RAIL_ENABLE, 0);

			changerail_timeout_cnt = 0;
			while(changerail_timeout_cnt < CHANGERAIL_TIMEOUT)
			{
				Task_sleep(10);
				changerail_timeout_cnt ++;

				regv = TTLRead();

				if((RailGetRailState() == LEFTRAIL && (regv & 0x03) == RIGHTRAIL) ||
					(RailGetRailState() == RIGHTRAIL && (regv & 0x03) == LEFTRAIL))
				{
					RailSetRailState(regv & 0x03);		/* 更新轨道状态 */
					changerail_timeout_cnt = 0;		/* 超时计数器清零 */
					break;
				}

			}

			TTLWriteBit(RAIL_ENABLE, 1);	/* 关闭电机使能 */

		}/*if(1 == changeRail)*/

	}/*while(1)*/
}

#if 0
static void vChangeRailTask(void)
{
	uint8_t state;
    uint16_t recvReg;
    p_msg_t sendmsg;
	static uint8_t step=0;
	static uint8_t preach=0;
	static uint8_t change_en=1;
	//static uint8_t SCount=0;
	uint8_t changeRailTimeoutCnt = 0;
	uint8_t uRail = 0;
	uint8_t lastuRail = 0;
	
	while(1)
	{
		Task_sleep(50);

		preach = 0;
		step = 0;

		if(RailGetReady() == 0)
		{
			changeRail = 0;
			continue;
		}

		if(1 == changeRail)
		{
			changeRail = 0;
			complete= 0;
			while(step != STEP_EXIT && change_en == 1)//正转270度
			{
				if(changeRailTimeoutCnt > CHANGERAIL_TIMEOUT)
				{
					/*
					*TODO:添加通信超时，发送急停消息，进入急停模式，并设置ErrorCode
					*/
					sendmsg = Message_getEmpty();
					sendmsg->type = error;
					sendmsg->data[0] = ERROR_CHANGERAIL_TIMEOUT;
					Message_post(sendmsg);
					changeRailTimeoutCnt = 0;
					RailSetReady(0);
					break;
				}


				switch(step)
				{
					case 0:
						//选择内部位置：Pn071
						Task_sleep(SLEEPTIME);
						state = ServoModbusWriteReg(0x02,0x0047,0x7FFF);


						if(state == MODBUS_ACK_OK)
						{
							//写成功，进行下一步
							step++;
							changeRailTimeoutCnt=0;
						}
						else
							changeRailTimeoutCnt ++;

						break;
					case 1:
						//使能伺服电机：Pn070
						Task_sleep(SLEEPTIME);
						state = ServoModbusWriteReg(0x02,0x0046,0x7FFE);

						if(state == MODBUS_ACK_OK)
						{
							//写成功，进行下一步
							step++;
							changeRailTimeoutCnt=0;
						}
						else
							changeRailTimeoutCnt ++;

						break;
					case 2:
						//触发内部位置，电机转动：Pn071
						Task_sleep(SLEEPTIME);

						if(RailGetRailState() == RIGHTRAIL)
							state = ServoModbusWriteReg(0x02,0x0047,0x7BFF);
						else
							state = ServoModbusWriteReg(0x02,0x0047,0x7AFF);

						if(state == MODBUS_ACK_OK)
						{
							//写成功，进行下一步
							step++;
							changeRailTimeoutCnt=0;
							Task_sleep(500);
						}
						else
							changeRailTimeoutCnt ++;

						break;
					case 3:
						//读取Dn018，判断bit3-Preach
						Task_sleep(SLEEPTIME);
						state = ServoModbusReadReg(0x02,0x0182,&recvReg);


						if(state == MODBUS_ACK_OK)
						{
							if(recvReg & 0x0008)//取Bit3 Preach,Bit 位为 0，表示功能为 ON 状态，为 1 则是 OFF 状态
							{
								//位置偏差，发出偏差报警
								preach=0;
							}
							else
							{
								//到达指定位置，进行下一步
								preach=1;
								step++;
								changeRailTimeoutCnt=0;
							}
						}
						else
							changeRailTimeoutCnt ++;

						break;
					case 4:
						//关闭伺服电机：Pn070
						Task_sleep(SLEEPTIME);
						state = ServoModbusWriteReg(0x02,0x0046,0x7FFF);

						if(state == MODBUS_ACK_OK)
						{
							if(preach)
							{
								if(RailGetRailState() == RIGHTRAIL)
									RailSetRailState(LEFTRAIL);
								else
									RailSetRailState(RIGHTRAIL);

								preach=0;//清除状态
							}
							else
							{
								change_en=0;//指令完成但位置偏差时，禁止变轨
							}
							step = STEP_EXIT;
							changeRailTimeoutCnt=0;
							complete= 1;
						}
						else
							changeRailTimeoutCnt ++;

						break;
				}/*end of switch(step)*/
			}/*end of while(step != STEP_EXIT && change_en == 1)*/

		}/* end of if(1 == changeRail)*/
		else
		{
			/* 每个变轨空闲周期内执行一次ModBus读操作，判定电机是否离线 */
			state = ServoModbusReadReg(0x02,0x0046,&recvReg);
			if(state == MODBUS_ACK_TIMEOUT)
				changeRailTimeoutCnt ++;
			else
				changeRailTimeoutCnt = 0;

			if(changeRailTimeoutCnt > CHANGERAIL_TIMEOUT)
			{
				/*
				*TODO:添加通信超时，发送急停消息，进入急停模式，并设置ErrorCode
				*/
				sendmsg = Message_getEmpty();
				sendmsg->type = error;
				sendmsg->data[0] = ERROR_CHANGERAIL_TIMEOUT;
				Message_post(sendmsg);
				changeRailTimeoutCnt = 0;
				RailSetReady(0);
				LogMsg("ChangeRail Servo Connect Timeout !!!\r\n");
				break;
			}
		}


	}
}
#endif

void ServoTaskInit()
{
    Task_Handle task;
	Task_Params taskParams;
	
	//初始化信号量
	ServoInitSem();

	//初始化串口
	UartNs550SetMode(SERVOR_MOTOR_UART, UART_RS485_MODE);
	UartNs550Init(SERVOR_MOTOR_UART,ServorUartIntrHandler);
	UartNs550RS485TxDisable(SERVOR_MOTOR_UART);

    UartNs550Recv(SERVOR_MOTOR_UART, &brakeUartDataObj.buffer, UART_REC_BUFFER_SIZE);


	Task_Params_init(&taskParams);
	taskParams.priority = 5;
	taskParams.stackSize = 2048;
    
	task = Task_create(ServoRecvDataTask, &taskParams, NULL);
	if (task == NULL) {
		System_printf("Task_create() failed!\n");
		BIOS_exit(0);
	}

    task = Task_create(ServoBrakeTask, &taskParams, NULL);
	if (task == NULL) {
		System_printf("Task_create() failed!\n");
		BIOS_exit(0);
	}

    task = Task_create(ServoChangeRailTask, &taskParams, NULL);
    if (task == NULL) {
        System_printf("Task_create() failed!\n");
        BIOS_exit(0);
    }

}


void RailSetRailState(uint8_t state)
{
	m_railCtrl.RailState = state;
}
uint8_t RailGetRailState()
{
	return m_railCtrl.RailState;
}

void BrakeSetBrake(uint8_t value)
{
	m_brakeCtrl.Brake = value;
}
uint8_t BrakeGetBrake()
{
	return m_brakeCtrl.Brake;
}
void BrakeSetReady(uint8_t value)
{
	m_brakeCtrl.BrakeReady = value;
}
uint8_t BrakeGetReady()
{
	return m_brakeCtrl.BrakeReady;
}
