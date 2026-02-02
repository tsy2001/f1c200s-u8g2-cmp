## link-libs
由于添加中文字体后程序体积非常大，上传调试需要等待很长时间。现把u8g2库和字体编译成动态链接库存放至/usr/lib/libu8g2*.so

## 编译
1、修改./user/Makefile  
  SYSROOT=修改成<你的buildroot绝对路径>/output/host/arm-buildroot-linux-uclibcgnueabi/sysroot  
2、make -j4  
  输出可执行文件在./output/bin/u8g2_hw_i2c_dvd  

