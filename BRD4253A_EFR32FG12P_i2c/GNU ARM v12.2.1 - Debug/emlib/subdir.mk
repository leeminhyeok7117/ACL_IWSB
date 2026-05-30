################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_cmu.c \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_core.c \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_emu.c \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_gpio.c \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_i2c.c \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_system.c 

OBJS += \
./emlib/em_cmu.o \
./emlib/em_core.o \
./emlib/em_emu.o \
./emlib/em_gpio.o \
./emlib/em_i2c.o \
./emlib/em_system.o 

C_DEPS += \
./emlib/em_cmu.d \
./emlib/em_core.d \
./emlib/em_emu.d \
./emlib/em_gpio.d \
./emlib/em_i2c.d \
./emlib/em_system.d 


# Each subdirectory must supply rules for building sources it contributes
emlib/em_cmu.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_cmu.c emlib/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DRETARGET_VCOM=1' '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"emlib/em_cmu.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

emlib/em_core.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_core.c emlib/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DRETARGET_VCOM=1' '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"emlib/em_core.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

emlib/em_emu.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_emu.c emlib/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DRETARGET_VCOM=1' '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"emlib/em_emu.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

emlib/em_gpio.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_gpio.c emlib/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DRETARGET_VCOM=1' '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"emlib/em_gpio.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

emlib/em_i2c.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_i2c.c emlib/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DRETARGET_VCOM=1' '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"emlib/em_i2c.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

emlib/em_system.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/emlib/src/em_system.c emlib/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DRETARGET_VCOM=1' '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"emlib/em_system.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


