################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/main_s1_xg12.c \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/common/src/sl_syscalls.c 

OBJS += \
./src/main_s1_xg12.o \
./src/sl_syscalls.o 

C_DEPS += \
./src/main_s1_xg12.d \
./src/sl_syscalls.d 


# Each subdirectory must supply rules for building sources it contributes
src/main_s1_xg12.o: ../src/main_s1_xg12.c src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"src/main_s1_xg12.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

src/sl_syscalls.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/common/src/sl_syscalls.c src/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"src/sl_syscalls.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


