/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * (C) Copyright 2021 Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <dwc3-uboot.h>
#include <usb.h>
#include <command.h>
#include <mmc.h>

DECLARE_GLOBAL_DATA_PTR;

#define CONFIG_GPIO_HUB_RST     136
#define CONFIG_GPIO_DIR_SWITCH  27

#ifdef CONFIG_USB_DWC3
static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_HIGH,
	.base = 0xfc000000,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.dis_u2_susphy_quirk = 1,
	.usb2_phyif_utmi_width = 16,
};

int usb_gadget_handle_interrupts(void)
{
	//dwc3_uboot_handle_interrupt(0);
	return 0;
}

int board_usb_init(int index, enum usb_init_type init)
{
	return 0;
	//return dwc3_uboot_init(&dwc3_device_data);
}
#endif

static int int_gpio_set_level(int gpio, char *desc, int val)
{
        int ret = 0;

        ret = gpio_request(gpio, desc);
        if (ret < 0) {
                printf("request for %d failed:%d\n", gpio, ret);
                return -1;
        }
        gpio_direction_output(gpio, val);
        gpio_free(gpio);

        return 0;
}

void fes_hub_rst(void)
{
#if 0
        int_gpio_set_level(CONFIG_GPIO_HUB_RST, "usb_hub_rst", 0);
        mdelay(20);
        int_gpio_set_level(CONFIG_GPIO_HUB_RST, "usb_hub_rst", 1);
#endif
        return ;
}

void dir_switch_sdio(void)
{
        int_gpio_set_level(CONFIG_GPIO_DIR_SWITCH, "switch_sdio", 1);

        return ;
}

void dp_power_enable(void)
{
        int_gpio_set_level(52, "dp_pwr_en", 1);

        return ;
}

int rk_board_init(void)
{
	dp_power_enable();

	return 0;
}

#ifndef CONFIG_SPL_BUILD

int load_logo_from_disk(char *filename, unsigned long addr, int size, int *len)
{
	int ret = -1;

	ret = emmc_load_file(filename, addr, size, len);
	if(ret)
		ret = tf_load_file(filename, addr, size, len);

	return ret;
}

static int do_load_version(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
        uint32_t vlen = 0;
        char cmd_mod[128] = {0};

        rockchip_show_logo();
        run_command("run distro_bootcmd;", -1);
        printf("Loading order: usb - sata - emmc\n");
        fes_hub_rst();
        run_command("usb reset;", -1);
        mdelay(1000);
        run_command("checkconf usb;", -1);
        if(!run_command("load usb 0:1 ${loadaddr_} ${bootdir}${image}", 0)) {
                run_command("load usb 0:1 ${initrd_addr} ${bootdir}${rd_file}", -1);
                vlen = simple_strtoul(env_get("filesize"), NULL, 16);
                run_command("load usb 0:1 ${fdt_addr_r} ${bootdir}${fdt_file}", -1);
                dir_switch_sdio();
                sprintf(cmd_mod, "unzip ${loadaddr_} ${loadaddr};booti ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", vlen);
                run_command(cmd_mod, -1);
                return 0;
        }

	run_command("checkconf sata;", -1);
        if(!run_command("load scsi 0:1 ${loadaddr_} ${bootdir}${image}", 0)) {
                run_command("load scsi 0:1 ${initrd_addr} ${bootdir}${rd_file}", -1);
                vlen = simple_strtoul(env_get("filesize"), NULL, 16);
                run_command("load scsi 0:1 ${fdt_addr_r} ${bootdir}${fdt_file}", -1);
                dir_switch_sdio();
                sprintf(cmd_mod, "unzip ${loadaddr_} ${loadaddr};booti ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", vlen);
                run_command(cmd_mod, -1);
                return 0;
        }

        run_command("checkconf mmc;", -1);
        if(!run_command("load mmc 0:1 ${loadaddr_} ${bootdir}${image}", 0)) {
                run_command("load mmc 0:1 ${initrd_addr} ${bootdir}${rd_file}", -1);
                vlen = simple_strtoul(env_get("filesize"), NULL, 16);
                run_command("load mmc 0:1 ${fdt_addr_r} ${bootdir}${fdt_file}", -1);
                dir_switch_sdio();
                sprintf(cmd_mod, "unzip ${loadaddr_} ${loadaddr};booti ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", vlen);
                run_command(cmd_mod, -1);
                return 0;
        }

        mdelay(3000);

        return 0;
}

U_BOOT_CMD(loadver, 4, 0, do_load_version,
           "Load version",
           "Booting\n"
);
#endif