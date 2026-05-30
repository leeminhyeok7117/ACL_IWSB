################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/Device/SiliconLabs/EFR32FG12P/Source/system_efr32fg12p.c 

S_UPPER_SRCS += \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/Device/SiliconLabs/EFR32FG12P/Source/GCC/startup_efr32fg12p.S 

OBJS += \
./CMSIS/EFR32FG12P/startup_efr32fg12p.o \
./CMSIS/EFR32FG12P/system_efr32fg12p.o 

C_DEPS += \
./CMSIS/EFR32FG12P/system_efr32fg12p.d 


# Each subdirectory must supply rules for building sources it contributes
CMSIS/EFR32FG12P/startup_efr32fg12p.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/Device/SiliconLabs/EFR32FG12P/Source/GCC/startup_efr32fg12p.S CMSIS/EFR32FG12P/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM Assembler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" '-DEFR32FG12P433F1024GL125=1' -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -x assembler-with-cpp -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

CMSIS/EFR32FG12P/system_efr32fg12p.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/Device/SiliconLabs/EFR32FG12P/Source/system_efr32fg12p.c CMSIS/EFR32FG12P/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DEFR32FG12P433F1024GL125=1' '-DDEBUG=1' -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/EFR32FG12_BRD4253A/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/bsp" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//hardware/kit/common/drivers" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -O0 -Wall -mno-sched-prolog -fno-builtin -ffunction-sections -fdata-sections -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -c -fmessage-length=0 -MMD -MP -MF"CMSIS/EFR32FG12P/system_efr32fg12p.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


