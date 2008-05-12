/*

	wiisd.c

	Hardware routines for reading and writing to the Wii's internal
	SD slot.

 Copyright (c) 2008
   Michael Wiedenbauer (shagkur)
   Dave Murphy (WinterMute)
	
 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation and/or
     other materials provided with the distribution.
  3. The name of the author may not be used to endorse or promote products derived
     from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#if defined(HW_RVL)

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <gcutil.h>
#include <ogc/ipc.h>
 
#define SDIO_HEAPSIZE				0x400
 
#define PAGE_SIZE512				512
 
#define SDIOHCR_HOSTCONTROL			0x28
 
#define SDIOHCR_HOSTCONTROL_4BIT	0x02
 
#define IOCTL_SDIO_WRITEHCREG		0x01
#define IOCTL_SDIO_READHCREG		0x02
#define IOCTL_SDIO_READCREG			0x03
#define IOCTL_SDIO_RESETCARD		0x04
#define IOCTL_SDIO_WRITECREG		0x05
#define IOCTL_SDIO_SETCLK			0x06
#define IOCTL_SDIO_SENDCMD			0x07
#define IOCTL_SDIO_SETBUSWIDTH		0x08
#define IOCTL_SDIO_READMCREG		0x09
#define IOCTL_SDIO_WRITEMCREG		0x0A
#define IOCTL_SDIO_GETSTATUS		0x0B
#define IOCTL_SDIO_GETOCR			0x0C
#define IOCTL_SDIO_READDATA			0x0D
#define IOCTL_SDIO_WRITEDATA		0x0E
 
#define SDIOCMD_TYPE_BC				1
#define SDIOCMD_TYPE_BCR			2
#define SDIOCMD_TYPE_AC				3
#define SDIOCMD_TYPE_ADTC			4
 
#define SDIO_RESPONSE_NONE			0
#define SDIO_RESPONSE_R1			1
#define SDIO_RESPONSE_R1B			2
#define SDIO_RESPOSNE_R2			3
#define SDIO_RESPONSE_R3			4
#define SDIO_RESPONSE_R4			5
#define SDIO_RESPONSE_R5			6
#define SDIO_RESPONSE_R6			7
 
#define SDIO_CMD_GOIDLE				0x00
#define SDIO_CMD_SENDRCA			0x03
#define SDIO_CMD_SELECT				0x07
#define SDIO_CMD_DESELECT			0x07
#define SDIO_CMD_SENDCSD			0x09
#define SDIO_CMD_SENDCID			0x0A
#define SDIO_CMD_SENDSTATUS			0x0D
#define SDIO_CMD_SETBLOCKLEN		0x10
#define SDIO_CMD_READBLOCK			0x11
#define SDIO_CMD_READMULTIBLOCK		0x12
#define SDIO_CMD_WRITEBLOCK			0x18
#define SDIO_CMD_WRITEMULTIBLOCK	0x19
#define SDIO_CMD_APPCMD				0x37
 
#define SDIO_ACMD_SETBUSWIDTH		0x06
#define SDIO_ACMD_SENDSCR			0x33
 
#define READ_BL_LEN					((u8)(__sd0_csd[5]&0x0f))
#define WRITE_BL_LEN				((u8)(((__sd0_csd[12]&0x03)<<2)|((__sd0_csd[13]>>6)&0x03)))
 
struct _sdiorequest
{
	u32 cmd;
	u32 cmd_type;
	u32 rsp_type;
	u32 arg;
	u32 blk_cnt;
	u32 blk_size;
	void *dma_addr;
	u32 isdma;
	u32 pad0;
};
 
struct _sdioresponse
{
	u32 rsp_fields[4];
};
 
static s32 hId = -1;
 
static s32 __sd0_fd = -1;
static u16 __sd0_rca = 0;
static s32 __sd0_initialized = 0;
static u8 __sd0_csd[16];
static u8 __sd0_cid[16];
 
static s32 __sdio_initialized = 0;
 
static u32 __sdio_status ATTRIBUTE_ALIGN(32) = 0;
static u32 __sdio_clockset ATTRIBUTE_ALIGN(32) = 0;
static char _sd0_fs[] ATTRIBUTE_ALIGN(32) = "/dev/sdio/slot0";
 
static s32 __sdio_sendcommand(u32 cmd,u32 cmd_type,u32 rsp_type,u32 arg,u32 blk_cnt,u32 blk_size,void *buffer,void *reply,u32 rlen)
{
	s32 i,ret;
	ioctlv *iovec;
	struct _sdiorequest *request;
	struct _sdioresponse *response;
 
	request = (struct _sdiorequest*)iosAlloc(hId,sizeof(struct _sdiorequest));
	if(request==NULL) return IPC_ENOMEM;
 
	response = (struct _sdioresponse*)iosAlloc(hId,sizeof(struct _sdioresponse));
	if(response==NULL) {
		iosFree(hId,request);
		return IPC_ENOMEM;
	}
	for(i=0;i<(sizeof(struct _sdioresponse)>>2);i++) response->rsp_fields[i] = 0;
 
	request->cmd = cmd;
	request->cmd_type = cmd_type;
	request->rsp_type = rsp_type;
	request->arg = arg;
	request->blk_cnt = blk_cnt;
	request->blk_size = blk_size;
	request->dma_addr = buffer;
	request->isdma = ((buffer!=NULL)?1:0);
	request->pad0 = 0;
 
	if(request->isdma) {
		iovec = (ioctlv*)iosAlloc(hId,(sizeof(ioctlv)*3));
		if(iovec==NULL) return IPC_ENOMEM;
 
		iovec[0].data = request;
		iovec[0].len = sizeof(struct _sdiorequest);
		iovec[1].data = buffer;
		iovec[1].len = (blk_size*blk_cnt);
		iovec[2].data = response;
		iovec[2].len = sizeof(struct _sdioresponse);
		ret = IOS_Ioctlv(__sd0_fd,IOCTL_SDIO_SENDCMD,2,1,iovec);
 
		iosFree(hId,iovec);
	} else
		ret = IOS_Ioctl(__sd0_fd,IOCTL_SDIO_SENDCMD,request,sizeof(struct _sdiorequest),response,sizeof(struct _sdioresponse));
 
	if(reply && !(rlen>16)) memcpy(reply,response,rlen);
 
	iosFree(hId,response);
	iosFree(hId,request);
 
	return ret;
}
 
static s32 __sdio_setclock(u32 set)
{
	s32 ret;
 
	__sdio_clockset = set;
	ret = IOS_Ioctl(__sd0_fd,IOCTL_SDIO_SETCLK,&__sdio_clockset,sizeof(u32),NULL,0);
 
	return ret;
}
static s32 __sdio_getstatus()
{
	s32 ret;
 
	__sdio_status = 0;
	ret = IOS_Ioctl(__sd0_fd,IOCTL_SDIO_GETSTATUS,NULL,0,&__sdio_status,sizeof(u32));
	if(ret<0) return ret;
 
	return __sdio_status;
}
 
static s32 __sdio_resetcard()
{
	s32 ret;
 
	__sd0_rca = 0;
	__sdio_status = 0;
	ret = IOS_Ioctl(__sd0_fd,IOCTL_SDIO_RESETCARD,NULL,0,&__sdio_status,sizeof(u32));
	if(ret<0) return ret;
 
	__sd0_rca = (u16)(__sdio_status>>16);
	return (__sdio_status&0xffff);
}
 
static s32 __sdio_gethcr(u8 reg,u8 *val)
{
	s32 ret;
	u32 *hcquery = NULL;
 
	if(val==NULL) return IPC_EINVAL;
 
	hcquery = (u32*)iosAlloc(hId,24);
	if(hcquery==NULL) return IPC_ENOMEM;
 
	*val = 0;
	hcquery[0] = reg;
	hcquery[1] = 0;
	hcquery[2] = 0;
	hcquery[3] = 1;
	hcquery[4] = 0;
	hcquery[5] = 0;
	__sdio_status = 0;
	ret = IOS_Ioctl(__sd0_fd,IOCTL_SDIO_READHCREG,(void*)hcquery,24,&__sdio_status,sizeof(u32));
	if(ret>=0) *val = (__sdio_status&0xff);
 
	iosFree(hId,hcquery);
	return ret;
}
 
static s32 __sdio_sethcr(u8 reg,u8 data)
{
	s32 ret;
	u32 *hcquery = NULL;
 
	hcquery = (u32*)iosAlloc(hId,24);
	if(hcquery==NULL) return IPC_ENOMEM;
 
	hcquery[0] = reg;
	hcquery[1] = 0;
	hcquery[2] = 0;
	hcquery[3] = 1;
	hcquery[4] = data;
	hcquery[5] = 0;
	ret = IOS_Ioctl(__sd0_fd,IOCTL_SDIO_WRITEHCREG,(void*)hcquery,24,NULL,0);
 
	iosFree(hId,hcquery);
	return ret;
}
 
static s32 __sdio_setbuswidth(u32 bus_width)
{
	s32 ret;
	u8 hc_reg = 0;
 
	ret = __sdio_gethcr(SDIOHCR_HOSTCONTROL,&hc_reg);
	if(ret<0) return ret;
 
	hc_reg &= ~SDIOHCR_HOSTCONTROL_4BIT;
	if(bus_width==4) hc_reg |= SDIOHCR_HOSTCONTROL_4BIT;
 
	return __sdio_sethcr(SDIOHCR_HOSTCONTROL,hc_reg);		
}
 
static s32 __sd0_getstatus()
{
	s32 ret;
	u32 status = 0;
 
	ret = __sdio_sendcommand(SDIO_CMD_SENDSTATUS,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1,(__sd0_rca<<16),0,0,NULL,&status,sizeof(u32));
	if(ret<0) return ret;
 
	return status;
}
 
static s32 __sd0_getrca()
{
	s32 ret;
 
	ret = __sdio_sendcommand(SDIO_CMD_SENDRCA,SDIOCMD_TYPE_BCR,SDIO_RESPONSE_R6,0,0,1,NULL,NULL,0);
	if(ret<0) return ret;
 
	__sd0_rca = (u16)(ret>>16);
	return (ret&0xffff);
}
 
static s32 __sd0_select()
{
	s32 ret;
 
	ret = __sdio_sendcommand(SDIO_CMD_SELECT,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1B,(__sd0_rca<<16),0,0,NULL,NULL,0);
 
	return ret;
}
 
static s32 __sd0_deselect()
{
	s32 ret;
 
	ret = __sdio_sendcommand(SDIO_CMD_DESELECT,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1B,0,0,0,NULL,NULL,0);
 
	return ret;
}
 
static s32 __sd0_setblocklength(u32 blk_len)
{
	s32 ret;
 
	ret = __sdio_sendcommand(SDIO_CMD_SETBLOCKLEN,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1,blk_len,0,0,NULL,NULL,0);
 
	return ret;
}
 
static s32 __sd0_setbuswidth(u32 bus_width)
{
	u16 val;
	s32 ret;
 
	val = 0x0000;
	if(bus_width==4) val = 0x0002;
 
	ret = __sdio_sendcommand(SDIO_CMD_APPCMD,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1,(__sd0_rca<<16),0,0,NULL,NULL,0);
	if(ret<0) return ret;
 
	ret = __sdio_sendcommand(SDIO_ACMD_SETBUSWIDTH,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1,val,0,0,NULL,NULL,0);
 
	return ret;		
}
 
static s32 __sd0_getcsd()
{
	s32 ret;
 
	ret = __sdio_sendcommand(SDIO_CMD_SENDCSD,SDIOCMD_TYPE_AC,SDIO_RESPOSNE_R2,(__sd0_rca<<16),0,0,NULL,__sd0_csd,16);
 
	return ret;
}
 
static s32 __sd0_getcid()
{
	s32 ret;
 
	ret = __sdio_sendcommand(SDIO_CMD_SENDCID,SDIOCMD_TYPE_AC,SDIO_RESPOSNE_R2,(__sd0_rca<<16),0,0,NULL,__sd0_cid,16);
 
	return ret;
}


static	bool __sd0_initio()
{
	s32 ret;
	//ret = __sdio_getstatus();
	ret = __sdio_resetcard();
	if(ret<0) {
		return false;
	}
 
	ret = __sdio_setbuswidth(4);
	if(ret<0) return false;
 
	ret = __sdio_setclock(1);
	if(ret<0) return false;
 
	ret = __sd0_select();
	if(ret<0) return false;
 
	ret = __sd0_setblocklength(PAGE_SIZE512);
	if(ret<0) {
		ret = __sd0_deselect();
		return false;
	}
 
	ret = __sd0_setbuswidth(4);
	if(ret<0) {
		ret = __sd0_deselect();
		return false;
	}
 
	__sd0_deselect();
	return true;
}

bool sdio_Deinitialize()
{
	if(__sd0_fd>=0)
		IOS_Close(__sd0_fd);
 
	if(hId>=0)
		iosDestroyHeap(hId);
 
	hId = -1;
	__sdio_initialized = 0;
	return true;
}

bool sdio_Startup()
{
	if(__sdio_initialized==1) return true;
 
	hId = iosCreateHeap(SDIO_HEAPSIZE);
	if(hId<0) return false;
 
	__sd0_fd = IOS_Open(_sd0_fs,1);

	if(__sd0_fd<0) {
		sdio_Deinitialize();
		return false;
	}
 
	if(__sd0_initio()==false) {
		sdio_Deinitialize();
		return false;
	}
	__sdio_initialized = 1;
	return true;
}
 
 
 
bool sdio_Shutdown()
{
	if(__sd0_initialized==0) return false;

	sdio_Deinitialize();
 
	__sd0_initialized = 0;
	return true;
}
 
bool sdio_ReadSectors(u32 sector,u32 numSectors,void* buffer)
{
	s32 ret;
	u8 *rbuf,*ptr;
	u32 blk_off;
 
	if(buffer==NULL) return false;
 
	ret = __sd0_select();
	if(ret<0) return false;
 
	rbuf = iosAlloc(hId,PAGE_SIZE512);
	if(rbuf==NULL) {
		__sd0_deselect();
		return false;
	}
 
	ptr = (u8*)buffer;
	while(numSectors>0) {
		blk_off = (sector*PAGE_SIZE512);
		ret = __sdio_sendcommand(SDIO_CMD_READMULTIBLOCK,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1,blk_off,1,PAGE_SIZE512,rbuf,NULL,0);
		if(ret>=0) {
			memcpy(ptr,rbuf,PAGE_SIZE512);
			ptr += PAGE_SIZE512;
			sector++;
			numSectors--;
		} else
			break;
	}
	__sd0_deselect();
 
	iosFree(hId,rbuf);
	return (ret>=0);
}
 
bool sdio_WriteSectors(u32 sector,u32 numSectors,const void* buffer)
{
	s32 ret;
	u8 *wbuf,*ptr;
	u32 blk_off;
 
	if(buffer==NULL) return false;
 
	ret = __sd0_select();
	if(ret<0) return false;
 
	wbuf = iosAlloc(hId,PAGE_SIZE512);
	if(wbuf==NULL) {
		__sd0_deselect();
		return false;
	}
 
	ptr = (u8*)buffer;
	while(numSectors>0) {
		blk_off = (sector*PAGE_SIZE512);
		memcpy(wbuf,ptr,PAGE_SIZE512);
		ret = __sdio_sendcommand(SDIO_CMD_WRITEMULTIBLOCK,SDIOCMD_TYPE_AC,SDIO_RESPONSE_R1,blk_off,1,PAGE_SIZE512,wbuf,NULL,0);
		if(ret>=0) {
			ptr += PAGE_SIZE512;
			sector++;
			numSectors--;
		} else 
			break;
	}
	__sd0_deselect();
 
	iosFree(hId,wbuf);
	return (ret>=0);
}
 
bool sdio_ClearStatus()
{
	return true;
}
 
bool sdio_IsInserted()
{
	return (__sd0_initialized==1);
}

#endif