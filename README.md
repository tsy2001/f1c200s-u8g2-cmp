## 编译
1、修改./user/Makefile  
  SYSROOT=修改成<你的buildroot绝对路径>/output/host/arm-buildroot-linux-musleabi/sysroot  
2、make -j4  
  输出可执行文件在./output/bin/u8g2_hw_i2c_dvd  

## 已知问题
3、播放FLAC、WAV音频文件不支持24Bit解码，仅支持16Bit  
原因：内核驱动程序Simple Audio Card不支持  
改进思路：应用程序转换一下格式。当前版本遇到24bit的音频文件直接跳过。  

2、不定时内核BUG警告  
现象：无，或者声音突然卡一下然后恢复，软件仍可继续运行  
原因：猜测解码MP3和FLAC持续占用CPU，导致Kernel准备进入idle空闲调度时，发现当前CPU仍处在atomic不可调度状态。已裁剪Kernel、超频CPU、DDR，故障频率有所降低  
改进思路：将解码再独立出一个线程？但线程同步开销会很大，F1C200S性能较弱，本人能力十分有限，期待大佬提供思路  
``` sh
[ 31.219646] BUG: scheduling while atomic: swapper/0/0xffff0000 
[ 31.225581] CPU: 0 PID: 0 Comm: swapper Not tainted 5.7.1+ #105 
[ 31.231534] Hardware name: Allwinner suniv Family 
[ 31.236305] [<c010b7c0>] (unwind_backtrace) from [<c0109530>] (show_stack+0x10/0x14) 
[ 31.244100] [<c0109530>] (show_stack) from [<c012f3bc>] (__schedule_bug+0x58/0x7c) 
[ 31.251738] [<c012f3bc>] (__schedule_bug) from [<c0472c04>] (__schedule+0x4c/0x3dc) 
[ 31.259447] [<c0472c04>] (__schedule) from [<c0473104>] (schedule_idle+0x60/0x7c) 
[ 31.266977] [<c0473104>] (schedule_idle) from [<c0132cc8>] (cpu_startup_entry+0xc/0x10) 
[ 31.275020] [<c0132cc8>] (cpu_startup_entry) from [<c0700d0c>] (start_kernel+0x344/0x3e0)
```
1、内核严重错误  
现象：声音突然卡一下，然后恢复，软件仍可继续运行，复现频率不高  
原因：不详，猜测libcdio内部问题?  
``` sh
[ 236.113792] ------------[ cut here ]------------ 
[ 236.118547] WARNING: CPU: 0 PID: 93 at block/blk-mq.c:1386 __blk_mq_run_hw_queue+0x98/0xf8 
[ 236.126835] CPU: 0 PID: 93 Comm: u8g2_hw_i2c_dvd Tainted: G W 5.7.1+ #105 
[ 236.134919] Hardware name: Allwinner suniv Family 
[ 236.139670] [<c010b7c0>] (unwind_backtrace) from [<c0109530>] (show_stack+0x10/0x14) 
[ 236.147447] [<c0109530>] (show_stack) from [<c0113ca4>] (__warn+0xb8/0xcc) 
[ 236.154376] [<c0113ca4>] (__warn) from [<c0113d30>] (warn_slowpath_fmt+0x78/0xac) 
[ 236.161893] [<c0113d30>] (warn_slowpath_fmt) from [<c02d2ebc>] (__blk_mq_run_hw_queue+0x98/0xf8) 
[ 236.170701] [<c02d2ebc>] (__blk_mq_run_hw_queue) from [<c02d2f68>] (__blk_mq_delay_run_hw_queue+0x38/0x9c) 
[ 236.180364] [<c02d2f68>] (__blk_mq_delay_run_hw_queue) from [<c02d043c>] (blk_execute_rq+0x40/0x74) 
[ 236.189416] [<c02d043c>] (blk_execute_rq) from [<c02e15c8>] (sg_io+0x22c/0x334) 
[ 236.196737] [<c02e15c8>] (sg_io) from [<c02e17d8>] (scsi_cdrom_send_packet+0x108/0x17c) 
[ 236.204763] [<c02e17d8>] (scsi_cdrom_send_packet) from [<c02e2010>] (scsi_cmd_blk_ioctl+0x3c/0x44) 
[ 236.213734] [<c02e2010>] (scsi_cmd_blk_ioctl) from [<c03a39ac>] (cdrom_ioctl+0x3c/0xd60) 
[ 236.221850] [<c03a39ac>] (cdrom_ioctl) from [<c0385f30>] (sr_block_ioctl+0x90/0xbc) 
[ 236.229537] [<c0385f30>] (sr_block_ioctl) from [<c01c91a4>] (vfs_ioctl+0x20/0x38) 
[ 236.237041] [<c01c91a4>] (vfs_ioctl) from [<c01c9808>] (ksys_ioctl+0xc0/0x7e0) 
[ 236.244284] [<c01c9808>] (ksys_ioctl) from [<c0100060>] (ret_fast_syscall+0x0/0x50) 
[ 236.251947] Exception stack(0xc0d61fa8 to 0xc0d61ff0) 
[ 236.257012] 1fa0: 00000006 b6d7e930 00000006 00005393 b6d7e930 b6d7e928 
[ 236.265187] 1fc0: 00000006 b6d7e930 0000000c 00000036 b6d7e95c b6ce4150 00000000 b6d7ea4c
[ 236.273357] 1fe0: b6f6bf78 b6d7e7f8 b6dadb20 b6ef7950
[ 236.278414] ---[ end trace 517d3c24f97923a3 ]---
```
