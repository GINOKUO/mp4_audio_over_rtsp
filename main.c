#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "rtp.h"

#define AAC_FILE    "test.aac"
#define CLIENT_PORT 9832








  
#include "stdafx.h"
#include "mp4.h"
#include <malloc.h>
#include <string.h>







struct AdtsHeader
{
    unsigned int syncword;  //12 bit 同步字 '1111 1111 1111'，说明一个ADTS帧的开始
    unsigned int id;        //1 bit MPEG 标示符， 0 for MPEG-4，1 for MPEG-2
    unsigned int layer;     //2 bit 总是'00'
    unsigned int protectionAbsent;  //1 bit 1表示没有crc，0表示有crc
    unsigned int profile;           //1 bit 表示使用哪个级别的AAC
    unsigned int samplingFreqIndex; //4 bit 表示使用的采样频率
    unsigned int privateBit;        //1 bit
    unsigned int channelCfg; //3 bit 表示声道数
    unsigned int originalCopy;         //1 bit 
    unsigned int home;                  //1 bit 

    /*下面的为改变的参数即每一帧都不同*/
    unsigned int copyrightIdentificationBit;   //1 bit
    unsigned int copyrightIdentificationStart; //1 bit
    unsigned int aacFrameLength;               //13 bit 一个ADTS帧的长度包括ADTS头和AAC原始流
    unsigned int adtsBufferFullness;           //11 bit 0x7FF 说明是码率可变的码流

    /* number_of_raw_data_blocks_in_frame
     * 表示ADTS帧中有number_of_raw_data_blocks_in_frame + 1个AAC原始帧
     * 所以说number_of_raw_data_blocks_in_frame == 0 
     * 表示说ADTS帧中有一个AAC数据块并不是说没有。(一个AAC原始帧包含一段时间内1024个采样及相关数据)
     */
    unsigned int numberOfRawDataBlockInFrame; //2 bit
};

static int parseAdtsHeader(uint8_t* in, struct AdtsHeader* res)
{
    static int frame_number = 0;
    memset(res,0,sizeof(*res));

    if ((in[0] == 0xFF)&&((in[1] & 0xF0) == 0xF0))
    {
        res->id = ((unsigned int) in[1] & 0x08) >> 3;
        printf("adts:id  %d\n", res->id);
        res->layer = ((unsigned int) in[1] & 0x06) >> 1;
        printf( "adts:layer  %d\n", res->layer);
        res->protectionAbsent = (unsigned int) in[1] & 0x01;
        printf( "adts:protection_absent  %d\n", res->protectionAbsent);
        res->profile = ((unsigned int) in[2] & 0xc0) >> 6;
        printf( "adts:profile  %d\n", res->profile);
        res->samplingFreqIndex = ((unsigned int) in[2] & 0x3c) >> 2;
        printf( "adts:sf_index  %d\n", res->samplingFreqIndex);
        res->privateBit = ((unsigned int) in[2] & 0x02) >> 1;
        printf( "adts:pritvate_bit  %d\n", res->privateBit);
        res->channelCfg = ((((unsigned int) in[2] & 0x01) << 2) | (((unsigned int) in[3] & 0xc0) >> 6));
        printf( "adts:channel_configuration  %d\n", res->channelCfg);
        res->originalCopy = ((unsigned int) in[3] & 0x20) >> 5;
        printf( "adts:original  %d\n", res->originalCopy);
        res->home = ((unsigned int) in[3] & 0x10) >> 4;
        printf( "adts:home  %d\n", res->home);
        res->copyrightIdentificationBit = ((unsigned int) in[3] & 0x08) >> 3;
        printf( "adts:copyright_identification_bit  %d\n", res->copyrightIdentificationBit);
        res->copyrightIdentificationStart = (unsigned int) in[3] & 0x04 >> 2;
        printf( "adts:copyright_identification_start  %d\n", res->copyrightIdentificationStart);
        res->aacFrameLength = (((((unsigned int) in[3]) & 0x03) << 11) |
                                (((unsigned int)in[4] & 0xFF) << 3) |
                                    ((unsigned int)in[5] & 0xE0) >> 5) ;
        printf( "adts:aac_frame_length  %d\n", res->aacFrameLength);
        res->adtsBufferFullness = (((unsigned int) in[5] & 0x1f) << 6 |
                                        ((unsigned int) in[6] & 0xfc) >> 2);
        printf( "adts:adts_buffer_fullness  %d\n", res->adtsBufferFullness);
        res->numberOfRawDataBlockInFrame = ((unsigned int) in[6] & 0x03);
        printf( "adts:no_raw_data_blocks_in_frame  %d\n", res->numberOfRawDataBlockInFrame);

        return 0;
    }
    else
    {
        printf("failed to parse adts header\n");
        return -1;
    }
}

static int createUdpSocket()
{
    int fd;
    int on = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    return fd;
}

static int rtpSendAACFrame(int socket, char* ip, int16_t port,
                            struct RtpPacket* rtpPacket, uint8_t* frame, uint32_t frameSize)
{
    int ret;

    rtpPacket->payload[0] = 0x00;
    rtpPacket->payload[1] = 0x10;
    rtpPacket->payload[2] = (frameSize & 0x1FE0) >> 5; //高8位
    rtpPacket->payload[3] = (frameSize & 0x1F) << 3; //低5位

    memcpy(rtpPacket->payload+4, frame, frameSize);
/*
int i;
for(i=0;i<frameSize+4;i++)
printf("%x ",rtpPacket->payload[i]);
*/
    ret = rtpSendPacket(socket, ip, port, rtpPacket, frameSize+4);
    if(ret < 0)
    {
        printf("failed to send rtp packet\n");
        return -1;
    }

    rtpPacket->rtpHeader.seq++;

    /*
     * 如果采样频率是44100
     * 一般AAC每个1024个采样为一帧
     * 所以一秒就有 44100 / 1024 = 43帧
     * 时间增量就是 44100 / 43 = 1025
     * 一帧的时间为 1 / 43 = 23ms
     */
    rtpPacket->rtpHeader.timestamp += 1025;

    return 0;
}

int main(int argc, char* argv[])
{
    //int fd;
    int ret;
    int socket;
    uint8_t* frame;
    struct AdtsHeader adtsHeader;
    struct RtpPacket* rtpPacket;
/*
    fd = open(AAC_FILE, O_RDONLY);
    if(fd < 0)
    {
        printf("failed to open %s\n", AAC_FILE);
        return -1;
    }    
*/
   int len;
   int buff_tmp[4] = {0, 0, 0, 0};
   int stsz_type[4] = {0x7a, 0x73, 0x74, 0x73};
   int stco_type[4] = {0x6f, 0x63, 0x74, 0x73};
   int offset = 0;
   int frame_count = 0;
   int v_frame_count = 0;
   int a_frame_count = 0;
   int count = 0;


   int stsz_count = 0;
   int stsz_video_flag = 0;
   int a_stsz_start_offset = -1;
   int a_stsz_sample_count;
   int a_stsz_sample_count_offset = -1;
   int a_stsz_sample_size[1024000];

   int stco_count = 0;
   int stco_video_flag = 0;
   int a_stco_start_offset = -1;
   int a_stco_entry_count;
   int a_stco_entry_count_offset = -1;
   int a_stco_chunk_offset[1024000];
   int gg= 0;
   int size = 0;
   int no = 1;

   stream_t* fd = NULL;
   fd = create_file_stream();
   if (stream_open(fd, "test.mp4", MODE_READ) == 0)
      return -1;


    socket = createUdpSocket();
    if(socket < 0)
    {
        printf("failed to create udp socket\n");
        return -1;
    }

    frame = (uint8_t*)malloc(5000);
    rtpPacket = malloc(5000);

    rtpHeaderInit(rtpPacket, 0, 0, 0, RTP_VESION, RTP_PAYLOAD_TYPE_AAC, 1, 0, 0, 0x32411);

    while(1)
    {
	/*
        printf("--------------------------------\n");

        ret = read(fd, frame, 7);
        if(ret <= 0)
        {
            lseek(fd, 0, SEEK_SET);
            continue;            
        }

        if(parseAdtsHeader(frame, &adtsHeader) < 0)
        {
            printf("parse err\n");
            break;
        }

        ret = read(fd, frame, adtsHeader.aacFrameLength-7);
        if(ret < 0)
        {
            printf("read err\n");
            break;
        }

        rtpSendAACFrame(socket, "127.0.0.1", CLIENT_PORT,
                        rtpPacket, frame, adtsHeader.aacFrameLength-7);

        usleep(23000);
	*/
	len = stream_read(fd, frame, 1);
 	buff_tmp[3] = buff_tmp[2];
		  buff_tmp[2] = buff_tmp[1];
		  buff_tmp[1] = buff_tmp[0];
		  buff_tmp[0] = frame[0];
//audio stsz
		  if(buff_tmp[0] == stsz_type[0] && buff_tmp[1] == stsz_type[1] && buff_tmp[2] == stsz_type[2] && buff_tmp[3] == stsz_type[3])
		  {

			if(stsz_video_flag == 1)
			{

			  a_stsz_sample_count_offset = offset + 9;
			  a_stsz_start_offset = a_stsz_sample_count_offset + 4;


			}
			stsz_video_flag++;
		  }

                  if(offset == a_stsz_sample_count_offset + count)
		  {
			if(count == 0)
			{
				a_stsz_sample_count = frame[0] << 24;

				count++;
			}
			else if(count == 1)
			{
				a_stsz_sample_count += frame[0] << 16;
				count++;
			}
			else if(count == 2)
			{
				a_stsz_sample_count += frame[0] << 8;
				count++;
			}
			else if(count == 3)
			{
				a_stsz_sample_count += frame[0];
				count = 0;

			}

		  }

		  if(offset == a_stsz_start_offset + count)
		  {
				
			if(count == 0)
			{
				a_stsz_sample_size[stsz_count] = frame[0] << 24;
				count++;
			}
			else if(count == 1)
			{
				a_stsz_sample_size[stsz_count] += frame[0] << 16;
				count++;
			}
			else if(count == 2)
			{
				a_stsz_sample_size[stsz_count] += frame[0] << 8;
				count++;
			}
			else if(count == 3)
			{
				a_stsz_sample_size[stsz_count] += frame[0];
				if(stsz_count < a_stsz_sample_count)
				{
					stsz_count++;
					a_stsz_start_offset += 4;
					count = 0;
				}
			}
		  }
		 //audio stco
 		 if(buff_tmp[0] == stco_type[0] && buff_tmp[1] == stco_type[1] && buff_tmp[2] == stco_type[2] && buff_tmp[3] == stco_type[3])
		  {
			if(stco_video_flag == 1)
			{
			  a_stco_entry_count_offset = offset + 5;
			  a_stco_start_offset = a_stco_entry_count_offset + 4;
			}
			stco_video_flag++;
		  }

                  if(offset == a_stco_entry_count_offset + count)
		  {
			if(count == 0)
			{
				a_stco_entry_count = frame[0] << 24;
				count++;
			}
			else if(count == 1)
			{
				a_stco_entry_count += frame[0] << 16;
				count++;
			}
			else if(count == 2)
			{
				a_stco_entry_count += frame[0] << 8;
				count++;
			}
			else if(count == 3)
			{
				a_stco_entry_count += frame[0];
				count = 0;
			}
			
		  }

		  if(offset == a_stco_start_offset + count)
		  {
				
			if(count == 0)
			{
				a_stco_chunk_offset[stco_count] = frame[0] << 24;
				count++;
			}
			else if(count == 1)
			{
				a_stco_chunk_offset[stco_count] += frame[0] << 16;
				count++;
			}
			else if(count == 2)
			{
				a_stco_chunk_offset[stco_count] += frame[0] << 8;
				count++;
			}
			else if(count == 3)
			{
				a_stco_chunk_offset[stco_count] += frame[0];
				count = 0;
				if(stco_count < a_stco_entry_count)
				{
					stco_count++;
					a_stco_start_offset += 4;
				}
			}
		  }

	  	  offset++;
	//printf("%d ",frame[0]);
	if(len == 0)
	 break;
    }
   stream_close(fd);
   destory_file_stream(fd);


	//printf("------- %d ------\n",a_stsz_sample_count);
offset = 0;
fd = create_file_stream();
if (stream_open(fd, "test.mp4", MODE_READ) == 0)
      return -1;


while(1)
   {

			//printf("-------offset  %d size %d------\n", a_stco_chunk_offset[frame_count],size);

		if(offset >= a_stco_chunk_offset[frame_count] + size)  
		{	 	
			//printf("------- %d ------\n", size);
			
		 	len = stream_read(fd, frame, a_stsz_sample_size[a_frame_count]);
			offset += a_stsz_sample_size[a_frame_count];
			size   += a_stsz_sample_size[a_frame_count];
			//printf("-------offset  %d size %d------\n", a_stco_chunk_offset[frame_count],a_stsz_sample_size[a_frame_count]);
			
			if(a_frame_count == 2000)
			{
			int i;
			for(i=0;i<a_stsz_sample_size[a_frame_count];i++)
			printf("%x ",frame[i]);
			break;
			}
			

			rtpSendAACFrame(socket, "127.0.0.1", CLIENT_PORT,rtpPacket, frame, a_stsz_sample_size[a_frame_count]);

		        usleep(23000);

			if(no == 1)
			{
				if(gg < 21)
				{
					gg++;
				}
				else
				{
					frame_count++;		
					gg = 0;	
					size = 0;
					no++;	
					//printf("\n------------------------------\n");
					//printf("%d ",a_stco_chunk_offset[frame_count]);
				}

			}
			else if( no % 2 == 0)
			{
				if(gg < 19)
				{
					gg++;
				}
				else
				{
					frame_count++;		
					gg = 0;	
					size = 0;
					no++;	
					//printf("\n------------------------------\n");
					//printf("%d ",a_stco_chunk_offset[frame_count]);
				}

			}
			else if( no % 2 == 1)
			{
				if(no == 467)
				{
					//printf("\n---------------%d---------------\n",a_stco_entry_count);
					if(gg < 12)
					{
						gg++;
					}
					else
					{
						frame_count++;		
						gg = 0;	
						size = 0;
						no++;	
						//printf("%d ",a_stco_chunk_offset[frame_count]);
					}
				}
				else{
					if(gg < 20)
					{
						gg++;
					}
					else
					{
						frame_count++;		
						gg = 0;	
						size = 0;
						no++;	
						//printf("\n------------------------------\n");
						//printf("%d ",a_stco_chunk_offset[frame_count]);
					}
				}
				
			}
			

			
			a_frame_count++;
			
		}
		
		else
		{
			len = stream_read(fd, frame, 1);	
			offset++;	
		}
		
		if (frame_count == a_stco_entry_count)
			break;


   }
   
   stream_close(fd);
   destory_file_stream(fd);

/*
int i;
for(i=0; i < a_stco_entry_count;i++)
		printf("%d ",a_stco_chunk_offset[i]);
*/


    //close(fd);
    close(socket);

    free(frame);
    free(rtpPacket);

    return 0;
}
