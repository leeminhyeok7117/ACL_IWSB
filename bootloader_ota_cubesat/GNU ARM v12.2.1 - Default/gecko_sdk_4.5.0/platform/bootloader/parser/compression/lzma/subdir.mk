################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/bootloader/parser/compression/lzma/LzmaDec.c 

OBJS += \
./gecko_sdk_4.5.0/platform/bootloader/parser/compression/lzma/LzmaDec.o 

C_DEPS += \
./gecko_sdk_4.5.0/platform/bootloader/parser/compression/lzma/LzmaDec.d 


# Each subdirectory must supply rules for building sources it contributes
gecko_sdk_4.5.0/platform/bootloader/parser/compression/lzma/LzmaDec.o: /home/lmh/SimplicityStudio/SDKs/gecko_sdk/platform/bootloader/parser/compression/lzma/LzmaDec.c gecko_sdk_4.5.0/platform/bootloader/parser/compression/lzma/subdir.mk
	@echo 'Building file: $<'
	@echo 'Invoking: GNU ARM C Compiler'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -std=c99 '-DEFR32FG12P433F1024GL125=1' '-DBTL_PARSER_SUPPORT_CUSTOM_TAGS=1' '-DBTL_PARSER_SUPPORT_LZMA=1' '-D_LZMA_SIZE_OPT=1' '-DBOOTLOADER_ENABLE=1' '-DBOOTLOADER_SECOND_STAGE=1' '-DSL_RAMFUNC_DISABLE=1' '-D__START=main' '-D__STARTUP_CLEAR_BSS=1' '-DBOOTLOADER_SUPPORT_INTERNAL_STORAGE=1' '-DBOOTLOADER_SUPPORT_STORAGE=1' '-DSL_BOARD_NAME="BRD4253A"' '-DSL_BOARD_REV="A03"' '-DHARDWARE_BOARD_DEFAULT_RF_BAND_915=1' '-DHARDWARE_BOARD_SUPPORTS_2_RF_BANDS=1' '-DHARDWARE_BOARD_SUPPORTS_RF_BAND_2400=1' '-DHARDWARE_BOARD_SUPPORTS_RF_BAND_915=1' '-DHFXO_FREQ=38400000' '-DSL_COMPONENT_CATALOG_PRESENT=1' '-DMBEDTLS_CONFIG_FILE=<sl_mbedtls_config.h>' '-DMBEDTLS_PSA_CRYPTO_CLIENT=1' '-DMBEDTLS_PSA_CRYPTO_CONFIG_FILE=<psa_crypto_config.h>' -I"/home/lmh/SimplicityStudio/v5_workspace/bootloader_ota_cubesat/config" -I"/home/lmh/SimplicityStudio/v5_workspace/bootloader_ota_cubesat/autogen" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/Device/SiliconLabs/EFR32FG12P/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader/parser/compression" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader/debug" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader/parser" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader/api" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader/core/flash" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/bootloader/security" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/CMSIS/Core/Include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/emlib/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/security/sl_component/sl_mbedtls_support/config" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/security/sl_component/sl_mbedtls_support/config/preset" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/security/sl_component/sl_mbedtls_support/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//util/third_party/mbedtls/include" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//util/third_party/mbedtls/library" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/security/sl_component/sl_psa_driver/inc" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//util/silicon_labs/silabs_core/memory_manager" -I"/home/lmh/SimplicityStudio/SDKs/gecko_sdk//platform/common/toolchain/inc" -Os -Wall -Wextra -ffunction-sections -fdata-sections -imacrossl_gcc_preinclude.h -mfpu=fpv4-sp-d16 -mfloat-abi=softfp --specs=nano.specs -c -fmessage-length=0 -MMD -MP -MF"gecko_sdk_4.5.0/platform/bootloader/parser/compression/lzma/LzmaDec.d" -MT"$@" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


