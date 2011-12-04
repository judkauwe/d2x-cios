/*   
	Custom IOS Module (FAT)

	Copyright (C) 2008 neimod.
	Copyright (C) 2009 WiiGator.
	Copyright (C) 2009 Waninkoko.
	Copyright (C) 2011 davebaol.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#include "ff.h"
#include "fs.h"
#include "fat_wrapper.h"
#include "ipc.h"
#include "mem.h"
#include "types.h"
#include "ehci.h"
#include "sdio.h"

/* Variables */
static FATFS fatFs[_VOLUMES] ATTRIBUTE_ALIGN(32);

PARTITION VolToPart[_VOLUMES]  ATTRIBUTE_ALIGN(32) = {
{0,4}, {1,4}
};

/* Buffer */
static char  lfnBuf[_MAX_LFN + 1] ATTRIBUTE_ALIGN(32);


s32 __FAT_OpenDir(DIR *dir, const char *dirpath)
{
	s32 ret;

	/* Open directory */
	ret = f_opendir(dir, dirpath);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_INVALID_NAME:

	// FIX:
	// Case uncommented in d2x v3beta6 for error code compatibility
	// to fix the message "corrupted data" in The Tower of Druaga.
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 __FAT_ReadDir(DIR *dir, FILINFO *fno)
{
	s32 ret;

	/* Loop */
	while (1) {
		/* Read entry */
		ret = f_readdir(dir, fno);
		if (ret)
			return FS_ENOENT;

		/* Read end */
		if (fno->fname[0] == '\0')
			return FS_ENOENT;

		/* Check entry */
		if (fno->fname[0] != '.')
			break;
	}

	return FS_SUCCESS;
}


s32 __FAT_MountPartition(u8 device, u8 partition)
{
	s32 ret;

	/* Set partition */
	VolToPart[device].pt = partition;

	/* Mount device */
	ret = f_mount(device, &fatFs[device]);
	if (!ret) {
		DIR d;
		char *root = (device == 0) ? "0:/" : "1:/";

		/* Open root directory */
		if (f_opendir(&d, root) == FR_OK)
			return FS_SUCCESS;

		/* Unmount device */
		f_mount(device, NULL);
	}

	/* Reset partition */
	VolToPart[device].pt = 4;

	return FS_EFATAL;
}


s32 FAT_Mount(u8 device, s32 partition)
{
	s32 ret, i;

	/* Initialize device */
	switch (device) {
	case 0:
		/* Initialize SDIO */
		ret = sdio_Startup();
		break;

	case 1:
		/* Initialize EHCI */
		ret = ehci_Init();
		break;

	default:
		/* Unknown device */
		ret = 0;
		break;
  }

	/* Device not found */
	if (!ret)
		return IPC_EINVAL;

	/* Mount required partition */
	if (partition >= 0 && partition < 4)
		return __FAT_MountPartition(device, partition);

	/* Find partition */
	for (i = 0; i < 4; i++) {
		ret = __FAT_MountPartition(device, i);
		if (!ret)
			return FS_SUCCESS;
	}

	return FS_EFATAL;
}

s32 FAT_Unmount(u8 device)
{
	s32 ret;

	/* Unmount device */
	ret = f_mount(device, NULL);
	if (ret)
		return FS_EFATAL;

	/* Reset partition */
	VolToPart[device].pt = 4;

	return FS_SUCCESS;
}

s32 FAT_Open(const char *path, u32 mode)
{
	FIL *fil = NULL;
	s32  ret;

	/* Allocate entry */
	fil = Mem_Alloc(sizeof(FIL));
	if (!fil)
		return IPC_ENOMEM;

	/* Open file */
	ret = f_open(fil, path, mode);
	if (ret) {
		/* Free entry */
		Mem_Free(fil);

		return FS_ENOENT;
	}

	return (u32)fil;
}

s32 FAT_Close(s32 fd)
{
	FIL *fil = (FIL *)fd;
	s32  ret;

	/* Close file */
	ret = f_close(fil);
	if (ret)
		return FS_EFATAL;

	/* Free memory */
	Mem_Free(fil);

	return FS_SUCCESS;
}

s32 FAT_Read(s32 fd, void *buffer, u32 len)
{
	FIL *fil = (FIL *)fd;

	u32 read;
	s32 ret;

	/* Read file */
	ret = f_read(fil, buffer, len, &read);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return read;

	case FR_DENIED:
		return FS_EACCESS;
	}

	return FS_EFATAL;
}

s32 FAT_Write(s32 fd, void *buffer, u32 len)
{
	FIL *fil = (FIL *)fd;

	u32 wrote;
	s32 ret;

	/* Write file */
	ret = f_write(fil, buffer, len, &wrote);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return wrote;

	case FR_DENIED:
		return FS_EACCESS;
	}

	return FS_EFATAL;
}

s32 FAT_Seek(s32 fd, s32 where, s32 whence)
{
	FIL *fil = (FIL *)fd;

	u32 offset;
	s32 ret;

	/* Calculate offset */
	switch (whence) {
	case SEEK_SET:
		offset = where;
		break;

	case SEEK_CUR:
		offset = fil->fptr  + where;
		break;

	case SEEK_END:
		offset = fil->fsize + where;
		break;

	default:
		// FIX:
 		// Modified in d2x v4beta2 to improve
 		// error code compatibility.
		return FS_EFATAL; //FS_EINVAL;
	}

	// FIX:
 	// Check added in d2x v4beta2 to prevent from increasing
 	// the file size when seeking out of the file.
	if(offset > f_size(fil))
		return FS_EFATAL;

	/* Seek file */
	ret = f_lseek(fil, offset);

	/* Error */
	if (ret)
		return FS_EFATAL;

	return offset;
}

s32 FAT_CreateDir(const char *dirpath)
{
	s32 ret;

	/* Create directory */
	ret = f_mkdir(dirpath);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_DENIED:
		return FS_EACCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_FILE:
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_CreateFile(const char *filepath)
{
	FIL fil;
	s32 ret;

	/* Create file */
	ret = f_open(&fil, filepath, FA_CREATE_NEW);

	/* Check result */
	switch (ret) {
	case FR_OK:
		f_close(&fil);
		return FS_SUCCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_FILE:
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_ReadDir(const char *dirpath, char *outbuf, u32 buflen, u32 *outlen, u32 entries, u8 lfn)
{
	DIR     dir;
	FILINFO fno;

	char *buffer = NULL;
	u32   cnt = 0, pos = 0;
	s32   ret;
	FIL   fil;
	
	// FIX:
	// Check added in d2x v4beta2 to improve error code compatibility.
	// Now FS_EFATAL is returned when the requested folder is an existing file.
	// With this fix some games like all Strong Bad episodes are working now.
	ret = f_open(&fil, dirpath, FA_OPEN_EXISTING);
	if (ret == FR_OK) {
		f_close(&fil);
		return FS_EFATAL;
	}

	/* Open directory */
	ret = __FAT_OpenDir(&dir, dirpath);
	if (ret)
		return ret;

	/* Allocate memory */
	if (buflen) {
		buffer = Mem_Alloc(buflen);
		if (!buffer)
			return IPC_ENOMEM;
	}

	/* Setup LFN */
	fno.lfname = lfnBuf;
	fno.lfsize = sizeof(lfnBuf);

	/* Read directory */
	while (!entries || (entries > cnt)) {
		u32 len;
		char *name;

		/* Read entry */
		ret = __FAT_ReadDir(&dir, &fno);
		if (ret)
			break;

		/* Get name */
		name = (*fno.lfname) ? fno.lfname : fno.fname;

		/* Get length */
		len = strnlen(name, _MAX_LFN);
		
		/* Skip entries that are too long for FS */
		if(!lfn && len > 12)
			continue;

		/* Copy entry */
		if (buffer) {
			char *ptr = buffer + pos;

			/* Copy filename */
			strncpy(ptr, name, len);
			ptr[len] = 0;

			/* Update position */
			pos += len + 1;
		}

		/* Increase counter */
		cnt++;
	}

	/* Copy entries */
	if (outbuf && buffer) {
		/* Copy buffer */
		memcpy(outbuf, buffer, buflen);

		/* Free buffer */
		Mem_Free(buffer);
	}

	/* Set value */
	*outlen = cnt;

	return FS_SUCCESS;
}

s32 FAT_Delete(const char *path)
{
	s32 ret;

	/* Delete object */
	ret = f_unlink(path);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_DENIED:
		return FS_EACCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_FILE:
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_DeleteDir(const char *dirpath)
{
	DIR     dir;
	FILINFO fno;

	s32 ret;

	/* Open directory */
	ret = __FAT_OpenDir(&dir, dirpath);
	if (ret)
		return ret;

	/* Setup LFN */
	fno.lfname = lfnBuf;
	fno.lfsize = sizeof(lfnBuf);

	/* Read directory */
	for (;;) {
		char  path[_MAX_LFN + 1];
		char *name;

		/* Read entry */
		ret = __FAT_ReadDir(&dir, &fno);
		if (ret)
			break;

		/* Get name */
		name = (*fno.lfname) ? fno.lfname : fno.fname;

		/* Generate path */
		strcpy(path, dirpath);
		strcat(path, "/");
		strcat(path, name);

		/* Entry is directory */
		if (fno.fattrib & AM_DIR)
			FAT_DeleteDir(path);

		/* Delete */
		ret = FAT_Delete(path);
		if (ret)
			return ret;
	}

	return FS_SUCCESS;
}

s32 FAT_Rename(const char *oldname, const char *newname)
{
	char *ptr;
	s32   ret;

	/* Skip prefix */
	ptr = strchr(newname, ':');
	if (ptr)
		newname = ptr + 1;

	/* Rename object */
	ret = f_rename(oldname, newname);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_PATH:
	case FR_NO_FILE:
		return FS_ENOENT;
	}

	return FS_EFATAL;
} 

s32 FAT_GetStats(const char *path, struct stats *stats)
{
	FILINFO fno;
	s32     ret;

	// FIX:
	// Initialization code added in d2x v4beta2 because members lfname 
	// and lfsize MUST be inited before using FILEINFO structure.
	// Now games like Max & the Magic Marker, FFCC My Life as a King
	// and FFCC My Life as a Darklord are working poperly.
	/* Setup LFN */
	fno.lfname = lfnBuf;
	fno.lfsize = sizeof(lfnBuf);

	/* Get stats */
	ret = f_stat(path, &fno);
	
	// FIX:
	// Error code compatibility improved in d2x v3beta6.
	// This way LIT doesn't stall anymore
	switch(ret) {
		case FR_OK:
			/* Fill info */
			if (stats) {
				stats->size   = fno.fsize;
				stats->date   = fno.fdate;
				stats->time   = fno.ftime;
				stats->attrib = fno.fattrib;
			}
			return FS_SUCCESS;
			
		case FR_NO_FILE:
		case FR_NO_PATH:
		case FR_INVALID_NAME:
			return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_GetFileStats(s32 fd, struct fstats *stats)
{
	FIL *fil = (FIL *)fd;

	/* No file */
	if (!fil->fs)
		return FS_EFATAL;

	/* Fill stats */
	stats->length = fil->fsize;
	stats->pos    = fil->fptr;

	return FS_SUCCESS;
}

s32 FAT_GetUsage(const char *dirpath, u64 *size, u32 *files)
{
	DIR     dir;
	FILINFO fno;

	u64 totalSz  = 0;
	u32 totalCnt = 0;

	s32 ret;

	/* Open directory */
	ret = __FAT_OpenDir(&dir, dirpath);
	if (ret)
		return ret;

	/* Setup LFN */
	fno.lfname = lfnBuf;
	fno.lfsize = sizeof(lfnBuf);

	/* Read directory */
	for (;;) {
		u64 fsize;
		u32 fcount;

		/* Read entry */
		ret = __FAT_ReadDir(&dir, &fno);
		if (ret)
			break;

		/* Entry is directory */
		if (fno.fattrib & AM_DIR) {
			char  path[_MAX_LFN + 1];
			char *name;

			/* Get name */
			name = (*fno.lfname) ? fno.lfname : fno.fname;

			/* Generate path */
			strcpy(path, dirpath);
			strcat(path, "/");
			strcat(path, name);

			/* Get directory usage */
			ret = FAT_GetUsage(path, &fsize, &fcount);
			if (ret)
				return ret;
		} else {
			/* Get file info */
			fcount = 1;
			fsize  = fno.fsize;
		}

		/* Update variables */
		totalCnt += fcount;
		totalSz  += fsize;
	}

	/* Set values */
	*files = totalCnt;
	*size  = totalSz;

	return FS_SUCCESS;
}