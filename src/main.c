/*
 * Copyright (c) 2011, NVIDIA CORPORATION
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <error.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/param.h>
#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>

#include "usb.h"
#include "nv3p.h"
#include "nv3p_status.h"
#include "aes-cmac.h"
#include "rcm.h"
#include "debug.h"

// tegra20 miniloader
#include "tegra20-miniloader.h"

// tegra30 miniloader
#include "tegra30-miniloader.h"

static int wait_status(nv3p_handle_t h3p);
static int send_file(nv3p_handle_t h3p, const char *filename);
static int download_miniloader(usb_device_t *usb, uint8_t *miniloader,
			       uint32_t size, uint32_t entry);
static void dump_platform_info(nv3p_platform_info_t *info);
static int download_bct(nv3p_handle_t h3p, char *filename);
static int download_bootloader(nv3p_handle_t h3p, char *filename,
			       uint32_t entry, uint32_t loadaddr);

enum cmdline_opts {
	OPT_BCT,
	OPT_BOOTLOADER,
	OPT_LOADADDR,
	OPT_ENTRYADDR,
	OPT_HELP,
	OPT_END,
};

static void usage(char *progname)
{
	fprintf(stderr, "usage: %s --bct=bctfile --bootloader=blfile --loadaddr=<loadaddr> --entryaddr=<entryaddr>\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "\tIf entryaddr is not specified, it's assumed to be the same as loadaddr\n");
	fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
	// discover devices
	uint8_t *msg_buff;
	uint64_t uid;
	int actual_len;
	usb_device_t *usb;
	uint32_t status;
	nv3p_platform_info_t info;
	nv3p_handle_t h3p;
	int ret;
	int c;
	int option_index = 0;
	char *bctfile = NULL;
	char *blfile = NULL;
	uint32_t loadaddr = 0;
	uint32_t entryaddr = 0;
	uint16_t devid;
	uint8_t *miniloader;
	uint32_t miniloader_size;
	uint32_t miniloader_entry;

	static struct option long_options[] = {
		[OPT_BCT]        = {"bct", 1, 0, 0},
		[OPT_BOOTLOADER] = {"bootloader", 1, 0, 0},
		[OPT_LOADADDR]   = {"loadaddr", 1, 0, 0},
		[OPT_ENTRYADDR]  = {"entryaddr", 1, 0, 0},
		[OPT_HELP]       = {"help", 0, 0, 0},
		[OPT_END]        = {0, 0, 0, 0}
	};

	// parse command line args
	while (1) {
		c = getopt_long(argc, argv, "h0",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			switch (option_index) {
			case OPT_BCT:
				bctfile = optarg;
				break;
			case OPT_BOOTLOADER:
				blfile = optarg;
				break;
			case OPT_LOADADDR:
				loadaddr = strtoul(optarg, NULL, 0);
				break;
			case OPT_ENTRYADDR:
				entryaddr = strtoul(optarg, NULL, 0);
				break;
			case OPT_HELP:
			default:
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if (bctfile == NULL) {
		fprintf(stderr, "BCT file must be specified\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if (blfile == NULL) {
		fprintf(stderr, "bootloader file must be specified\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if (loadaddr == 0) {
		fprintf(stderr, "loadaddr must be specified\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if (entryaddr == 0) {
		entryaddr = loadaddr;
	}

	printf("bct file: %s\n", bctfile);
	printf("booloader file: %s\n", blfile);
	printf("load addr 0x%x\n", loadaddr);
	printf("entry addr 0x%x\n", entryaddr);

	usb = usb_open(USB_VENID_NVIDIA, &devid);
	if (!usb)
		error(1, errno, "could not open USB device");
	printf("device id: 0x%x\n", devid);

	ret = usb_read(usb, (uint8_t *)&uid, sizeof(uid), &actual_len);
	if (ret)
		error(1, ret, "USB transfer failure");
	if (actual_len < sizeof(uid))
		error(1, errno, "USB read truncated");

	printf("uid:  0x%" PRIx64 "\n", uid);

	// create query version message
	rcm_create_msg(RCM_CMD_QUERY_RCM_VERSION, NULL, 0, NULL, 0, &msg_buff);

	// write query version message to device
	ret = usb_write(usb, msg_buff, rcm_get_msg_len(msg_buff));
	if (ret)
		error(1, errno, "USB transfer failure");
	free(msg_buff);
	msg_buff = NULL;

	// read response
	ret = usb_read(usb, (uint8_t *)&status, sizeof(status), &actual_len);
	if (ret)
		error(1, ret, "USB transfer failure");
	if (actual_len < sizeof(status))
		error(1, EIO, "USB read truncated");
	printf("RCM version: %d.%d\n", RCM_VERSION_MAJOR(status),
	       RCM_VERSION_MINOR(status));

	printf("downloading miniloader to target...\n");
	if ((devid & 0xff) == USB_DEVID_NVIDIA_TEGRA20) {
		miniloader = miniloader_tegra20;
		miniloader_size = sizeof(miniloader_tegra20);
		miniloader_entry = TEGRA20_MINILOADER_ENTRY;
	} else if ((devid & 0xff) == USB_DEVID_NVIDIA_TEGRA30) {
		miniloader = miniloader_tegra30;
		miniloader_size = sizeof(miniloader_tegra30);
		miniloader_entry = TEGRA30_MINILOADER_ENTRY;
	} else {
		error(1, ENODEV, "unknown tegra device: 0x%x", devid);
	}
	ret = download_miniloader(usb, miniloader, miniloader_size,
				  miniloader_entry);
	if (ret)
		error(1, ret, "Error downloading miniloader");
	printf("miniloader downloaded successfully\n");

	// device may have re-enumerated, so reopen USB
	usb_close(usb);
	usb = usb_open(USB_VENID_NVIDIA, &devid);
	if (!usb)
		error(1, errno, "could not open USB device");

	// now that miniloader is up, start nv3p protocol
	ret = nv3p_open(&h3p, usb);
	if (ret)
		error(1, errno, "3p open failed");

	// get platform info and dump it
	ret = nv3p_cmd_send(h3p, NV3P_CMD_GET_PLATFORM_INFO, (uint8_t *)&info);
	if (ret)
		error(1, errno, "retreiving platform info");
	ret = wait_status(h3p);
	if (ret)
		error(1, errno, "wait status after platform info");
	dump_platform_info(&info);

	if (info.op_mode != RCM_OP_MODE_DEVEL &&
	    info.op_mode != RCM_OP_MODE_ODM_OPEN)
		error(1, ENODEV, "device is not in developer or open mode, cannot flash");

	// download the BCT
	ret = download_bct(h3p, bctfile);
	if (ret) {
		error(1, ret, "error downloading bct: %s", bctfile);
	}

	// download the bootloader
	ret = download_bootloader(h3p, blfile, entryaddr, loadaddr);
	if (ret)
		error(1, ret, "error downloading bootloader: %s", blfile);

	nv3p_close(h3p);
	usb_close(usb);

	return 0;
}

static int wait_status(nv3p_handle_t h3p)
{
	int ret;
	uint32_t cmd;
	nv3p_cmd_status_t *status_arg = 0;

	ret = nv3p_cmd_recv(h3p, &cmd, (void **)&status_arg);
	if (ret)
		goto fail;
	if (cmd != NV3P_CMD_STATUS) {
		ret = EIO;
		goto fail;
	}
	if (status_arg->code != nv3p_status_ok) {
		ret = EIO;
		goto fail;
	}

	nv3p_ack(h3p);

	return 0;

fail:
	if(status_arg) {
		printf("bootloader status: (code: %d) message: %s flags: %d\n",
		       status_arg->code, status_arg->msg, status_arg->flags );
	}
	return ret;
}


/*
* send_file: send data present in file "filename" to nv3p server.
*/
static int send_file(nv3p_handle_t h3p, const char *filename)
{
	int ret;
	uint8_t *buf = 0;
	uint32_t size;
	uint64_t total;
	uint32_t bytes;
	uint64_t count;
	char *spinner = "-\\|/";
	int spin_idx = 0;
	int fd = -1;
	struct stat sb;

#define NVFLASH_DOWNLOAD_CHUNK (1024 * 64)

	printf("sending file: %s\n", filename );

	fd = open(filename, O_RDONLY, 0);
	if (fd < 0) {
		ret = errno;
		goto fail;
	}

	if (fstat(fd, &sb) < 0) {
		ret = errno;
		goto fail;
	}

	total = sb.st_size;

	buf = malloc( NVFLASH_DOWNLOAD_CHUNK );
	if (!buf) {
		ret = ENOMEM;
		goto fail;
	}

	count = 0;
	while(count != total) {
		size = (uint32_t)MIN(total - count, NVFLASH_DOWNLOAD_CHUNK);

		bytes = read(fd, buf, size);
		if (bytes < 0) {
			ret = errno;
			goto fail;
		}

		ret = nv3p_data_send(h3p, buf, bytes);
		if (ret)
			goto fail;

		count += bytes;

		printf("\r%c %" PRIu64 "/%" PRIu64" bytes sent", spinner[spin_idx],
			count, total);
		spin_idx = (spin_idx + 1) % 4;
	}
	printf("\n%s sent successfully\n", filename);

#undef NVFLASH_DOWNLOAD_CHUNK

	close(fd);
	free(buf);
	return 0;

fail:
	if (fd != -1)
		close(fd);
	if (buf)
		free(buf);
	return ret;
}


static int download_miniloader(usb_device_t *usb, uint8_t *miniloader,
			       uint32_t size, uint32_t entry)
{
	uint8_t *msg_buff;
	int ret;
	uint32_t status;
	int actual_len;

	// download the miniloader to the bootrom
	rcm_create_msg(RCM_CMD_DL_MINILOADER,
		       (uint8_t *)&entry, sizeof(entry), miniloader, size,
		       &msg_buff);
	ret = usb_write(usb, msg_buff, rcm_get_msg_len(msg_buff));
	if (ret)
		goto fail;
	ret = usb_read(usb, (uint8_t *)&status, sizeof(status), &actual_len);
	if (ret)
		goto fail;
	if (actual_len < sizeof(status)) {
		ret = EIO;
		goto fail;
	}
	if (status != 0) {
		ret = EIO;
		goto fail;
	}

	ret = 0;
fail:
	free(msg_buff);
	return ret;
}

static void dump_platform_info(nv3p_platform_info_t *info)
{
	printf("Chip UID:                0x%" PRIx64 "\n", info->uid);
	printf("Chip ID:                 0x%x\n", (uint32_t)info->chip_id.id);
	printf("Chip ID Major Version:   0x%x\n", (uint32_t)info->chip_id.major);
	printf("Chip ID Minor Version:   0x%x\n", (uint32_t)info->chip_id.minor);
	printf("Chip SKU:                0x%x", info->sku);

	// Convert chip sku to chip name as per chip id
	char *chip_name = NULL;
	if (info->chip_id.id == 0x20) {
		switch (info->sku) {
		case TEGRA2_CHIP_SKU_AP20:   chip_name = "ap20"; break;
		case TEGRA2_CHIP_SKU_T20:    chip_name = "t20"; break;
		case TEGRA2_CHIP_SKU_T25SE:  chip_name = "t25se"; break;
		case TEGRA2_CHIP_SKU_AP25:   chip_name = "ap25"; break;
		case TEGRA2_CHIP_SKU_T25:    chip_name = "t25"; break;
		case TEGRA2_CHIP_SKU_AP25E:  chip_name = "ap25e"; break;
		case TEGRA2_CHIP_SKU_T25E:   chip_name = "t25e"; break;
		default: chip_name = "unknown"; break;
		}
	} else if (info->chip_id.id == 0x30) {
		switch (info->sku) {
		case TEGRA3_CHIP_SKU_AP30:   chip_name = "ap30"; break;
		case TEGRA3_CHIP_SKU_T30:    chip_name = "t30"; break;
		case TEGRA3_CHIP_SKU_T30S:   chip_name = "t30s"; break;
		default: chip_name = "unknown"; break;
		}
	} else {
		chip_name = "unknown";
	}
	printf(" (%s)\n", chip_name);

	printf("Boot ROM Version:        0x%x\n", info->version);
	printf("Boot Device:             0x%x", info->boot_device);
	char *boot_dev = NULL;
	switch(info->boot_device) {
	case NV3P_DEV_TYPE_NAND:            boot_dev = "NAND"; break;
	case NV3P_DEV_TYPE_EMMC:            boot_dev = "EMMC"; break;
	case NV3P_DEV_TYPE_SPI:             boot_dev = "SPI"; break;
	case NV3P_DEV_TYPE_IDE:             boot_dev = "IDE"; break;
	case NV3P_DEV_TYPE_NAND_X16:        boot_dev = "NAND x16"; break;
	case NV3P_DEV_TYPE_SNOR:            boot_dev = "SNOR"; break;
	case NV3P_DEV_TYPE_MUX_ONE_NAND:    boot_dev = "Mux One NAND"; break;
	case NV3P_DEV_TYPE_MOBILE_LBA_NAND: boot_dev = "Mobile LBA NAND"; break;
	default:                            boot_dev = "unknown"; break;
	}
	printf(" (%s)\n", boot_dev);

	printf("Operating Mode:          0x%x", info->op_mode);
	char *op_mode = NULL;
	switch(info->op_mode) {
	case RCM_OP_MODE_DEVEL:             op_mode = "developer mode"; break;
	case RCM_OP_MODE_ODM_OPEN:          op_mode = "odm open mode"; break;
	default:                            op_mode = "unknown"; break;
	}
	printf(" (%s)\n", op_mode);

	printf("Device Config Strap:     0x%x\n", info->dev_conf_strap);
	printf("Device Config Fuse:      0x%x\n", info->dev_conf_fuse);
	printf("SDRAM Config Strap:      0x%x\n", info->sdram_conf_strap);
}


static int download_bct(nv3p_handle_t h3p, char *filename)
{
	int ret;
	nv3p_cmd_dl_bct_t arg;
	struct stat sb;

	ret = stat(filename, &sb);
	if (ret)
		return ret;
	arg.length = sb.st_size;

	ret = nv3p_cmd_send(h3p, NV3P_CMD_DL_BCT, (uint8_t *)&arg);
	if (ret)
		return ret;
	ret = send_file(h3p, filename);
	if (ret)
		return ret;

	ret = wait_status(h3p);
	if (ret)
		error(1, errno, "wait status after sending bct");

	return 0;
}


static int download_bootloader(nv3p_handle_t h3p, char *filename,
			       uint32_t entry, uint32_t loadaddr)
{
	int ret;
	nv3p_cmd_dl_bl_t arg;
	int fd;
	struct stat sb;

	fd = open(filename, O_RDONLY, 0);
	if (fd < 0) {
		dprintf("error opening %s for reading\n", filename);
		return errno;
	}

	ret = fstat(fd, &sb);
	if (ret) {
		dprintf("error on fstat of %s\n", filename);
		return ret;
	}
	arg.length = sb.st_size;
	close(fd);

	arg.entry = entry;
	arg.address = loadaddr;

	ret = nv3p_cmd_send(h3p, NV3P_CMD_DL_BL, (uint8_t *)&arg);
	if (ret) {
		dprintf("error sending 3p bootloader download command\n");
		return ret;
	}

	ret = wait_status(h3p);
	if (ret) {
		dprintf("error waiting for status on bootloader dl\n");
		return ret;
	}

	// send the bootloader file
	ret = send_file(h3p, filename);
	if (ret) {
		dprintf("error downloading bootloader\n");
		return ret;
	}

	return 0;
}