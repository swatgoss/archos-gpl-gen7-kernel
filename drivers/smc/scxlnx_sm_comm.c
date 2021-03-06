/*
 * Copyright (c) 2006-2008 Trusted Logic S.A.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <asm/div64.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/interrupt.h>
#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/jiffies.h>

#include "scxlnx_defs.h"
#include "scxlnx_sm_comm.h"
#include "scxlnx_util.h"
#include "scxlnx_conn.h"

#include "scx_public_crypto.h"

#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
#include <linux/freezer.h>
#endif


#define SECURE_TRACE

#ifdef SECURE_TRACE
#define secure_dprintk  printk
#else
#define secure_dprintk(args...)  do { ; } while (0)
#endif /* defined (SECURE_TRACE) */


/*---------------------------------------------------------------------------
 * Internal Constants
 *---------------------------------------------------------------------------*/

/*
 * Trace offset
 */
#define RPC_TRACE_OFFSET 0xC00 // traces start at offset 3072


/*
 * RPC Identifiers
 */
#define RPC_ID_SEC_STORAGE    0x00
#define RPC_ID_DMA            0x01
#define RPC_ID_SMODULE        0x02

/*
 * RPC Return values
 */
#define RPC_SUCCESS                    0x00000000
#define RPC_ERROR_GENERIC              0xFFFF0000
#define RPC_ERROR_BAD_PARAMETERS       0xFFFF0006
#define RPC_ERROR_OUT_OF_MEMORY        0xFFFF000C
#define RPC_ERROR_CONNECTION_PROTOCOL  0xFFFF3020

/*
 * RPC Commands
 */
#define RPC_CMD_YIELD                        0x00
#define RPC_CMD_INIT                         0x01
#define RPC_CMD_TRACE                        0x02

#define RPC_CMD_PUBLIC_CRYPTO_INIT               0x10
#define RPC_CMD_PUBLIC_CRYPTO_UPDATE             0x11
#define RPC_CMD_PUBLIC_CRYPTO_FINAL              0x12
#define RPC_CMD_PUBLIC_CRYPTO_TERMINATE          0x13
#define RPC_CMD_PUBLIC_CRYPTO_ENABLE_CLOCK       0x14
#define RPC_CMD_PUBLIC_CRYPTO_INVALIDATE_SESSION 0x15


/*
 * SE entry flags
 */
#define FLAG_START_HAL_CRITICAL     0x4
#define FLAG_IRQFIQ_MASK            0x3
#define FLAG_IRQ_ENABLE             0x2
#define FLAG_FIQ_ENABLE             0x1

/*
 * HAL API Identifiers
 */
#define API_HAL_PA_LOAD             15
#define API_HAL_PA_UNLOAD_ALL       17
#define API_HAL_SDP_RUNTIME_INIT    19
#define API_HAL_SEC_RPC_INIT        21
#define API_HAL_SEC_RAM_RESIZE      26
#define API_HAL_CONTEXT_SAVE_RESTORE  25
#define API_HAL_KM_CRC_READ               34


/*
 * HAL API return codes
 */
#define API_HAL_RET_VALUE_OK        0x0
#define API_HAL_RET_VALUE_SDP_RUNTIME_INIT_ERROR        0x20

/*
 * HAL API RAM Resize values
 */
#define SEC_RAM_SIZE_48KB     0x0000C000
#define SEC_RAM_SIZE_60KB     0x0000F000
#define SEC_RAM_SIZE_64KB     0x00010000


/*
 * Time constants
 */
#define TIME_IMMEDIATE ((u64) 0x0000000000000000ULL)
#define TIME_INFINITE  ((u64) 0xFFFFFFFFFFFFFFFFULL)

/*
 * The name of the polling thread.
 */
#define SCXLNX_SM_COMM_POLLING_THREAD_NAME      SCXLNX_DEVICE_BASE_NAME

/*
 * The nOperationID field of a message points to this structure.
 * It is used to identify the thread that triggered the message transmission
 * Whoever reads an answer can wake up that thread using the completion event
 */
typedef struct {
   struct completion sAnswerEvent;
   SCX_ANSWER_MESSAGE* pAnswer;

   u32 nSendingThreadAvailable;
} SCXLNX_SM_ANSWER_STRUCT;


typedef struct {
   void  *pCertificate;
   void  *pParameters;
   void  *pResults;

} NS_PA_INFO;


static SCXLNX_SM_COMM_MONITOR* g_pSMComm = NULL;
static bool g_L1SharedReady = false;



/*---------------------------------------------------------------------------
 * Extern declarations
 *---------------------------------------------------------------------------*/

extern u32 pub2sec_bridge_entry(u32 appl_id, u32 proc_ID, u32 flag, u32 paNextArgs);
extern u32 rpc_handler(u32 p1, u32 p2, u32 p3, u32 p4);
extern u32 v7_flush_kern_cache_all(void);
#ifdef SMC_CACHE_OPTIM
extern u32 v7_dma_flush_range(u32 nVAStart, u32 nVAEnd);
#endif /* SMC_CACHE_OPTIM */

/*----------------------------------------------------------------------------
 * Function responsible for formatting the parameters to pass
 * from NS-World to S-World.
 *----------------------------------------------------------------------------*/
u32 SEC_ENTRY_pub2sec_dispatcher(u32 appl_id, u32 proc_ID, u32 flag, u32 nArgs,
											u32 arg1, u32 arg2, u32 arg3, u32 arg4)
{
   u32 return_value = 0;
   u32* pArgs = NULL;

	dprintk(KERN_INFO "SEC_ENTRY_pub2sec_dispatcher: ApplId=0x%x, ProcId=0x%x, flag=0x%x, args=%u\n",
				appl_id, proc_ID, flag, nArgs);

   /*
    * We need a physically contiguous buffer to pass parameters to the SE
    */
   pArgs = (u32*)internal_kmalloc(sizeof(u32) * 5, GFP_KERNEL);
	if (pArgs == NULL) {
      return -ENOMEM;
   }

	pArgs[0] = nArgs;
	pArgs[1] = arg1;
	pArgs[2] = arg2;
	pArgs[3] = arg3;
	pArgs[4] = arg4;

	dprintk(KERN_INFO "SEC_ENTRY_pub2sec_dispatcher: args=0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				pArgs[0], pArgs[1], pArgs[2], pArgs[3], pArgs[4]);

   /*
    * OMAP3430 Secure ROM Code Functional Specification:
    *    L2 Cache is not used by SW which runs in Secure Mode.
    *    Thus, the non-Secure World�s software must ensure that any data
    *    in L2 Cache are coherent with memory before feeding such data to the
    *    Secure World for processing.
    */
   v7_flush_kern_cache_all();

	return_value = pub2sec_bridge_entry(appl_id, proc_ID, flag, __pa(pArgs));

   internal_kfree(pArgs);

   return return_value;
}


/*---------------------------------------------------------------------------
 * atomic operation definitions
 *---------------------------------------------------------------------------*/

/*
 * Atomically reads the value of the 32-bit register in the SM
 * communication buffer, taking endianness issues into account.
 */
static inline u32 SCXLNXSMCommReadReg32(
      const u32 *pValue)
{
   return *(const volatile u32 *)pValue;
}


/*
 * Atomically overwrites the value of the 32-bit register at nOffset in the SM
 * communication buffer pointed to by pCommBuffer with nValue, taking endianness
 * issues into accout.
 */
static inline void SCXLNXSMCommWriteReg32(
      void *pCommBuffer,
      u32 nValue)
{
   *((volatile u32 *)pCommBuffer) = nValue;
}


/*----------------------------------------------------------------------------*/
/*
 * Free the PA buffers.
 * Check the protocol version (returned by the PA).
 */
static u32 SCXLNXSMCommRPCInit(SCXLNX_SM_COMM_MONITOR* pSMComm)
{
   u32 nProtocolVersion;
   u32 nRPCError = RPC_SUCCESS;

   dprintk(KERN_INFO "SCXLNXSMCommRPCInit(%p)\n", pSMComm);

	if (pSMComm->pPABuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommRPCInit(%p) : PA Buffer released\n", pSMComm);
      internal_kfree(pSMComm->pPABuffer);
      pSMComm->pPABuffer = NULL;
   }

	if (pSMComm->pPAInfo != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommRPCInit(%p) : PA Info released\n", pSMComm);
      internal_kfree(pSMComm->pPAInfo);
      pSMComm->pPAInfo = NULL;
   }

   nProtocolVersion = *((u32*)(pSMComm->pL0SharedBuffer));

	if ((GET_PROTOCOL_MAJOR_VERSION(nProtocolVersion)) != SCX_SM_S_PROTOCOL_MAJOR_VERSION) {
      printk(KERN_ERR "SMC: Unsupported SMC Protocol PA Major Version (0x%02X, Expected 0x%02X) !\n",
            GET_PROTOCOL_MAJOR_VERSION(nProtocolVersion),
            GET_PROTOCOL_MAJOR_VERSION(SCX_SM_S_PROTOCOL_MAJOR_VERSION));
      nRPCError = RPC_ERROR_CONNECTION_PROTOCOL;
	} else {
      nRPCError = RPC_SUCCESS;
   }

   return nRPCError;
}




/*----------------------------------------------------------------------------
 * SMC related operations
 *----------------------------------------------------------------------------*/

/*
 * Atomically updates the nSyncSerial_N and sTime_N register
 * nSyncSerial_N and sTime_N modifications are thread safe
 */
static inline void SCXLNXSMCommSetCurrentTime(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   u32 nNewSyncSerial;
   struct timeval now;
   u64 sTime64;

   /*
    * lock the structure while updating the L1 shared memory fields
    */
   spin_lock(&(pSMComm->lock));

   /* read nSyncSerial_N and change the TimeSlot bit field */
   nNewSyncSerial = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nSyncSerial_N) + 1;

   do_gettimeofday(&now);
   sTime64 = now.tv_sec;
   sTime64 = (sTime64 * 1000) + (now.tv_usec / 1000);

   /* Write the new sTime and nSyncSerial into shared memory */
   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->sTime_N[nNewSyncSerial & SCX_SM_SYNC_SERIAL_TIMESLOT_N].nTime[0], (u32) (sTime64 & 0xFFFFFFFF));
   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->sTime_N[nNewSyncSerial & SCX_SM_SYNC_SERIAL_TIMESLOT_N].nTime[1], (u32) (sTime64 >> 32));
   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->nSyncSerial_N, nNewSyncSerial);

   spin_unlock(&(pSMComm->lock));
}

/*----------------------------------------------------------------------------*/
/*
 * Performs the specific read timeout operation
 * The difficulty here is to read atomically 2 u32
 * values from the L1 shared buffer.
 * This is guaranteed by reading before and after the operation
 * the timeslot given by the SM
 */
static inline void SCXLNXSMCommReadTimeout(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      u64 *pTime)
{
   u32 nSyncSerial_S_initial = 0;
   u32 nSyncSerial_S_final = 1;
   u64 sTime;

   spin_lock(&pSMComm->lock);

	while (nSyncSerial_S_initial != nSyncSerial_S_final) {
      nSyncSerial_S_initial = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nSyncSerial_S);
      sTime = (u64) SCXLNXSMCommReadReg32(&pSMComm->pBuffer->sTimeout_S[nSyncSerial_S_initial&1].nTime[1]);
      sTime = (sTime << 32) +
            (u64) SCXLNXSMCommReadReg32(&pSMComm->pBuffer->sTimeout_S[nSyncSerial_S_initial&1].nTime[0]);

      nSyncSerial_S_final = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nSyncSerial_S);
   }

   spin_unlock(&pSMComm->lock);

   *pTime = sTime;
}


/*
 * Unlocks the physical memory pages
 * and frees the pages that need to
 */
void SCXLNXSMCommReleaseDescriptor(
      SCXLNX_SHMEM_DESC *pShmemDesc,
      u32 nFullCleanup)
{
	switch (pShmemDesc->nAllocType) {
      case SCXLNX_SHMEM_ALLOC_TYPE_NONE:
         /* Nothing to do */
         break;

      case SCXLNX_SHMEM_ALLOC_TYPE_REGISTER:
		if (pShmemDesc->ppPages != NULL) {
            u32 i;

			for (i = 0; i < pShmemDesc->nAllocatedBufferSize; i++) {
				if (nFullCleanup && !PageReserved(pShmemDesc->ppPages[i])) {
                  SetPageDirty(pShmemDesc->ppPages[i]);
               }
               internal_page_cache_release(pShmemDesc->ppPages[i]);
            }

            internal_kfree(pShmemDesc->ppPages);
            pShmemDesc->ppPages = NULL;
         }
         break;

      case SCXLNX_SHMEM_ALLOC_TYPE_KMALLOC:
		if (pShmemDesc->pAllocatedBuffer != NULL) {
            internal_kfree(pShmemDesc->pAllocatedBuffer);
            pShmemDesc->pAllocatedBuffer = NULL;
         }
         break;

      case SCXLNX_SHMEM_ALLOC_TYPE_PAGES:
		if (pShmemDesc->pAllocatedBuffer != NULL) {
            u32 nOffset;

            /* Mark the pages as unreserved */
			for (nOffset = 0; nOffset < pShmemDesc->nAllocatedBufferSize; nOffset += PAGE_SIZE) {
#ifdef KERNEL_2_6_27
               clear_page_locked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#else
               ClearPageLocked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#endif
            }

            internal_free_pages((unsigned long)pShmemDesc->pAllocatedBuffer, (unsigned long)pShmemDesc->nBufferOrder);
            pShmemDesc->pAllocatedBuffer = NULL;
         }
         break;
   }

   pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_NONE;
}


/*
 * Make sure the pages are allocated. If not allocated, do it
 * Locks down the physical memory pages
 * Verifies the memory attributes depending on pFlags
 */
int SCXLNXSMCommFillDescriptor(
      SCXLNX_SHMEM_DESC *pShmemDesc,
      u32 *pnBufferAddr,
      u32 nBufferSize,
      u32 *pnFlags)
{
   int nError = 0;

   if (  (*pnBufferAddr != 0)
       /* In this case (direct access): - try to access the memory directly
                                        - otherwise allocate a new buffer to avoid fork issues */
	&& (((*pnFlags) & (SCX_SHMEM_TYPE_DIRECT | SCX_SHMEM_TYPE_DIRECT_FORCE)) != 0)) {
		/*
      /*
       * This is a buffer registration attempt...
       */
      unsigned long nBufferVAddr = (unsigned long)(*pnBufferAddr);
      unsigned long end = (nBufferVAddr + nBufferSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
      unsigned long start = nBufferVAddr >> PAGE_SHIFT;
      int nPagesMapped, i;
      unsigned long nPhysAddr;
      unsigned long nPhysAddrTmp;
      u32 nBufferOffset;

      pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_REGISTER;

      /* Number of pages */
      pShmemDesc->nAllocatedBufferSize = end - start;
      pShmemDesc->ppPages = internal_kmalloc(
            pShmemDesc->nAllocatedBufferSize * sizeof(*(pShmemDesc->ppPages)), GFP_KERNEL);
		if (pShmemDesc->ppPages == NULL) {
         nError = -ENOMEM;
         goto next;
      }

		dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Registering 0x%x: %u pages\n",
            pShmemDesc,
            (u32)nBufferVAddr,
            pShmemDesc->nAllocatedBufferSize);

      down_read(&(current->mm->mmap_sem));
      nPagesMapped = internal_get_user_pages(
            current,
            current->mm,
            nBufferVAddr,
            pShmemDesc->nAllocatedBufferSize,
            1, /* write */
            0, /* force */
            pShmemDesc->ppPages,
            NULL);
      up_read(&(current->mm->mmap_sem));

		if (nPagesMapped < 0) {
			dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : get_user_pages failed [%d] !\n",
                  pShmemDesc, nPagesMapped);
         /* This is done to release properly the already mapped pages (i.e. none) */
         pShmemDesc->nAllocatedBufferSize = 0;
         nError = nPagesMapped;
         goto next;
      }

		if ((u32)nPagesMapped != pShmemDesc->nAllocatedBufferSize) {
			dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : get_user_pages got %d pages from %u !\n",
                  pShmemDesc, nPagesMapped, pShmemDesc->nAllocatedBufferSize);
         /* This is done to release properly the already mapped pages */
         pShmemDesc->nAllocatedBufferSize = (u32)nPagesMapped;
         nError = -EFAULT;
         goto next;
      }

      /* Check that the buffer is physically contiguous */
      nPhysAddr = page_to_phys(pShmemDesc->ppPages[0]);
      start = nPhysAddr;

		dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Page  0: PhysAddr=0x%x\n",
            pShmemDesc, (u32)nPhysAddr);

		for (i = 1; i < nPagesMapped; i++) {
         start += PAGE_SIZE;
         nPhysAddrTmp = page_to_phys(pShmemDesc->ppPages[i]);

			dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Page %2d: PhysAddr=0x%x\n",
               pShmemDesc, i, (u32)nPhysAddrTmp);

			if (start != nPhysAddrTmp) {
            nError = -EINVAL;
				dprintk(KERN_WARNING "SCXLNXSMCommFillDescriptor(%p) : Buffer not physically contiguous !\n",
                     pShmemDesc);
            goto next;
         }
      }

      nBufferOffset = (u32)(nBufferVAddr & ~PAGE_MASK);

      pShmemDesc->pBuffer = (u8*)(page_address(pShmemDesc->ppPages[0])) + nBufferOffset;
      pShmemDesc->nBufferSize = nBufferSize;

		dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Registered Mapped Addr=0x%x (Offset=%u)\n",
            pShmemDesc, (u32)pShmemDesc->pBuffer, nBufferOffset);

      /* Update the buffer address with the physical address */
      *pnBufferAddr = (u32)nPhysAddr + nBufferOffset;

      return 0;
   }

next:
	if (nError != 0) {
      SCXLNXSMCommReleaseDescriptor(pShmemDesc, 0);

      /*
       * Registration failed so go on with the allocation method.
       */
		dprintk(KERN_WARNING "SCXLNXSMCommFillDescriptor(%p) : Register failed -> Allocation\n",
            pShmemDesc);

      *pnBufferAddr = 0;
   }

	if (*pnBufferAddr == 0) {
      pShmemDesc->nBufferSize = nBufferSize;

      /* Default allocation method is kmalloc */
      pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_KMALLOC;

      /* Works out the best allocation function */
		if (pShmemDesc->nBufferSize <= 4 * PAGE_SIZE) {
         pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_PAGES;
		} else {
         u32 nOrder;

			for (nOrder = 0; nOrder < 16; nOrder++) {
				if (pShmemDesc->nBufferSize == (0x1 << nOrder)*PAGE_SIZE) {
               pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_PAGES;
               break;
            }
         }
      }

		switch (pShmemDesc->nAllocType) {
         case SCXLNX_SHMEM_ALLOC_TYPE_KMALLOC:
            /* Will be rounded up to a page boundary */
            pShmemDesc->nAllocatedBufferSize = pShmemDesc->nBufferSize + PAGE_SIZE;
            pShmemDesc->pAllocatedBuffer = internal_kmalloc(
                  pShmemDesc->nAllocatedBufferSize, GFP_KERNEL);
			if (pShmemDesc->pAllocatedBuffer == NULL) {
               nError = -ENOMEM;
				dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : Out of memory for buffer (%u bytes)\n",
                        pShmemDesc, pShmemDesc->nAllocatedBufferSize);
               goto error;
            }

            /* Round it up to the page bondary */
            pShmemDesc->pBuffer = (u8*)PAGE_ALIGN((unsigned long)pShmemDesc->pAllocatedBuffer);
            pShmemDesc->nBufferPhysAddr = virt_to_phys(pShmemDesc->pBuffer);

            /*
             * __GFP_ZERO is not allowed in the allocation method,
             * so do it manually...
             */
            memset(pShmemDesc->pBuffer, 0, pShmemDesc->nBufferSize);

			dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : kmalloc: Size=%u, Alloc={0x%x->0x%x}\n",
                  pShmemDesc,
                  pShmemDesc->nAllocatedBufferSize,
                  (u32)pShmemDesc->pAllocatedBuffer,
                  (u32)pShmemDesc->pBuffer);
            break;

         case SCXLNX_SHMEM_ALLOC_TYPE_PAGES:
         {
            u32 nOffset;

            pShmemDesc->nBufferOrder = 0;
            pShmemDesc->nAllocatedBufferSize = (0x1 << pShmemDesc->nBufferOrder) * PAGE_SIZE;

			while (pShmemDesc->nAllocatedBufferSize < pShmemDesc->nBufferSize) {
               pShmemDesc->nBufferOrder ++;
               pShmemDesc->nAllocatedBufferSize = (0x1 << pShmemDesc->nBufferOrder) * PAGE_SIZE;
            }

            pShmemDesc->pAllocatedBuffer = (u8*)internal_get_free_pages(
                  GFP_KERNEL | __GFP_ZERO, pShmemDesc->nBufferOrder);
			if (pShmemDesc->pAllocatedBuffer == NULL) {
               nError = -ENOMEM;
				dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : Out of memory for buffer (%u bytes, order=%u)\n",
                        pShmemDesc,
                        pShmemDesc->nAllocatedBufferSize,
                        pShmemDesc->nBufferOrder);
               goto error;
            }

            /* Mark the pages as reserved */
			for (nOffset = 0; nOffset < pShmemDesc->nBufferSize; nOffset += PAGE_SIZE) {
#ifdef KERNEL_2_6_27
               set_page_locked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#else
               SetPageLocked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#endif
            }
            pShmemDesc->pBuffer = pShmemDesc->pAllocatedBuffer;
            pShmemDesc->nBufferPhysAddr = virt_to_phys(pShmemDesc->pBuffer);

			dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : get_free_pages: Size=%u, Order=%u, Alloc=0x%x\n",
                  pShmemDesc,
                  pShmemDesc->nAllocatedBufferSize,
                  pShmemDesc->nBufferOrder,
                  (u32)pShmemDesc->pAllocatedBuffer);
            break;
         }

         default:
            BUG_ON(true);  /* To make the compiler happy */
            break;
      }
   }

   /* Update the buffer address with the physical address */
   *pnBufferAddr = pShmemDesc->nBufferPhysAddr;

	dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Success: PhysAddr=0x%x\n",
         pShmemDesc, *pnBufferAddr);

   return 0;

error:
   SCXLNXSMCommReleaseDescriptor(pShmemDesc, 0);
   return nError;
}


/*----------------------------------------------------------------------------
 * Standard communication operations
 *----------------------------------------------------------------------------*/

/*
 * Returns a non-zero value if the specified S-timeout has expired, zero
 * otherwise.
 *
 * The placeholder referenced to by pnRelativeTimeoutJiffies gives the relative timeout
 * from now in jiffies. It is set to zero if the S-timeout has expired, or to MAX_SCHEDULE_TIMEOUT
 * if the S-timeout is infinite.
 */
static int SCXLNXSMCommTestSTimeout(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      u64 sTimeout,
      signed long *pnRelativeTimeoutJiffies)
{
   struct timeval now;
   u64 sTime64;

   *pnRelativeTimeoutJiffies = 0;

   /* immediate timeout */
	if (sTimeout == TIME_IMMEDIATE) {
      return 1;
   }

   /* infinite timeout */
	if (sTimeout == TIME_INFINITE) {
      dprintk(KERN_DEBUG "SCXLNXSMCommTestSTimeout: timeout is infinite\n");
      *pnRelativeTimeoutJiffies = MAX_SCHEDULE_TIMEOUT;
      return 0;
   }

   do_gettimeofday(&now);
   sTime64 = now.tv_sec;
   /* will not overflow as operations are done on 64bit values */
   sTime64 = (sTime64 * 1000) + (now.tv_usec / 1000);

   /* timeout expired */
	if (sTime64 >= sTimeout) {
      dprintk(KERN_DEBUG "SCXLNXSMCommTestSTimeout: timeout expired\n");
      return 1;
   }

   /*
    * finite timeout, compute pnRelativeTimeoutJiffies
    */
   /* will not overflow as sTime64 < sTimeout */
   sTimeout -= sTime64;

   /* guaranty *pnRelativeTimeoutJiffies is a valid timeout */
	if ((sTimeout >> 32) != 0) {
      *pnRelativeTimeoutJiffies = MAX_JIFFY_OFFSET;
	} else {
      *pnRelativeTimeoutJiffies = msecs_to_jiffies((unsigned int) sTimeout);
   }

   dprintk(KERN_DEBUG "SCXLNXSMCommTestSTimeout: timeout is 0x%lx\n", *pnRelativeTimeoutJiffies);
   return 0;
}

/* Forward Declaration */
static int SCXLNXSMCommSendMessage(
      SCXLNX_SM_COMM_MONITOR *pTZComm,
      SCX_COMMAND_MESSAGE* pMessage,
      SCXLNX_CONN_MONITOR* pConn,
      int bKillable);

/*----------------------------------------------------------------------------*/
/*
 * Reads out all the available answers sent by the SModule.
 * For each answer read, wakes up the corresponding emitting thread
 * by using the nOperationID
 */
static void SCXLNXSMCommConsumeAnswers(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   u32 nFirstAnswer;
   u32 nFirstFreeAnswer;

start:
   spin_lock(&(pSMComm->lock));

   nFirstFreeAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeAnswer);
   nFirstAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstAnswer);

	if (nFirstAnswer != nFirstFreeAnswer) {
		do {
         SCX_ANSWER_MESSAGE* pComAnswer;
         SCXLNX_SM_ANSWER_STRUCT* pAnswerStructure;

         pComAnswer = &pSMComm->pBuffer->sAnswerQueue[nFirstAnswer%32];
         pAnswerStructure = (SCXLNX_SM_ANSWER_STRUCT*)pComAnswer->nOperationID;

         SCXLNXDumpAnswer(pComAnswer);

         (void)memcpy(
            pAnswerStructure->pAnswer,
            pComAnswer,
            sizeof(*pComAnswer));

         nFirstAnswer++;
         SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->nFirstAnswer, nFirstAnswer);

			if (pAnswerStructure->nSendingThreadAvailable == 1) {
            complete(&pAnswerStructure->sAnswerEvent);
			} else 	{
				/*
            /*
             * The sending thread has received the SIGKILL signal,
             * clean up structure ourselves
             */
            dprintk(KERN_INFO "SCXLNXTZCommConsumeAnswers : sending thread killed\n");

            /*
             * We need to record the successfull creation of a device context
             */
				if ((pAnswerStructure->pAnswer->nMessageType == SCX_MESSAGE_TYPE_CREATE_DEVICE_CONTEXT) && (pAnswerStructure->pAnswer->nSChannelStatus == S_SUCCESS)) {
               SCX_COMMAND_MESSAGE sMessage;
               int nError;
               SCXLNX_SM_ANSWER_STRUCT* pAnswerStructureDC = pAnswerStructure;

               spin_unlock(&(pSMComm->lock));

               dprintk(KERN_INFO "SCXLNXSMCommConsumeAnswers(%p) : sending DESTROY_CONTEXT\n", pSMComm);

               pAnswerStructureDC->nSendingThreadAvailable = 0;
               sMessage.nMessageType = SCX_MESSAGE_TYPE_DESTROY_DEVICE_CONTEXT;
               sMessage.nOperationID = (u32)pAnswerStructureDC;
               sMessage.sBody.sDestroyDeviceContextMessage.hDeviceContext
                  = pAnswerStructureDC->pAnswer->sBody.sCreateDeviceContextAnswer.hDeviceContext;

               nError = SCXLNXSMCommSendMessage(
                           pSMComm,
                           &sMessage,
                           NULL,
                           0);
               /* SCXLNXSMCommSendMessage cannot return an error because
                  it's not killable and not within a connection */
               BUG_ON(nError != 0);

               /* need to complete the job */
               goto start;
            }

            internal_kfree(pAnswerStructure);
         }
		} while (nFirstAnswer != nFirstFreeAnswer);
   }

   spin_unlock(&(pSMComm->lock));
}

/*----------------------------------------------------------------------------*/
/*
 * Implements the entry-point of the SM polling threads.
 */
static int SCXLNXSMCommPollingThread(
      void *pParam)
{
   /*
    * Implementation note:
    *    The paInfo will be freed through a RPC call at the beginning
    *    of the PA entry in the SE.
    */
   int  nError;
   SCXLNX_SM_COMM_MONITOR *pSMComm = (SCXLNX_SM_COMM_MONITOR *)pParam;

	dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : Starting\n", pSMComm);

   /* Call daemonize() to remove any user space mem maps and signal handlers */
   daemonize(SCXLNX_SM_COMM_POLLING_THREAD_NAME);

   /* PA call */
   nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_PA_LOAD,
                                          0,
                                          FLAG_IRQ_ENABLE|FLAG_FIQ_ENABLE|FLAG_START_HAL_CRITICAL,
                                          1,
														__pa(pSMComm->pPAInfo),
														0, 0, 0);
	if (nError == API_HAL_RET_VALUE_OK) {
      nError = ((SCHANNEL_L0_BUFFER_OUTPUT*)(pSMComm->pL0SharedBuffer))->nL1Status;
      if (nError == S_ERROR_SDP_RUNTIME_INIT_ADDR_CHECK_FAIL) {
			printk(KERN_ERR "SMC: BackingStore and BackExtStorage addresses differs between initialization and configuration file.\n");
		} else if (nError == 0) {
			dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : SMC PA ended successfully\n",
               pSMComm);
		} else {
         printk(KERN_WARNING "SMC: PA ended with an error [0x%X]\n", nError);
      }
	} else {
      printk(KERN_ERR "SMC: PA load failed [0x%X]\n", nError);
   }

   test_and_clear_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags));
   test_and_clear_bit(SCXLNX_SM_COMM_FLAG_TERMINATING, &(pSMComm->nFlags));

   SCXLNXSMCommStop(pSMComm);

	dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : Signaling polling thread death\n",
            pSMComm);
   complete(&(pSMComm->pollingThreadDeath));

	dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : End\n", pSMComm);
   return nError;
}

/*----------------------------------------------------------------------------*/

/*
 * Implements the SM polling routine.
 */
static u32 SCXLNXSMCommPollingRoutine(SCXLNX_SM_COMM_MONITOR* pSMComm)
{
   DEFINE_WAIT(wait);
   u64 sTimeout;
   signed long nRelativeTimeoutJiffies;

begin:
   {
      /*
       * Check that the SM communication is still alive.
       */

		if (test_bit(SCXLNX_SM_COMM_FLAG_TERMINATING, &(pSMComm->nFlags)) != 0) {
			dprintk(KERN_INFO "SCXLNXSMCommPollingRoutine(%p) : Flag Terminating is set\n",
                  pSMComm);
         goto end;
      }

      prepare_to_wait(&pSMComm->waitQueue, &wait, TASK_INTERRUPTIBLE);

      /*
       * Consume the available answers, if any.
       */
      SCXLNXSMCommConsumeAnswers(pSMComm);

      /*
       * Check S-timeout.
       */
      SCXLNXSMCommReadTimeout(pSMComm, &sTimeout);

		if (SCXLNXSMCommTestSTimeout(pSMComm, sTimeout, &nRelativeTimeoutJiffies) == 0) {
         u32 nFirstCommand, nFirstFreeCommand;
         u32 nFirstAnswer, nFirstFreeAnswer;

         /* Lock concurrent access to the buffer while reading it */
         spin_lock(&(pSMComm->lock));
         nFirstCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstCommand);
         nFirstFreeCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeCommand);
         nFirstAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstAnswer);
         nFirstFreeAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeAnswer);
         spin_unlock(&(pSMComm->lock));

			if ((nFirstCommand == nFirstFreeCommand) && (nFirstAnswer == nFirstFreeAnswer)) {
				if (nRelativeTimeoutJiffies == MAX_SCHEDULE_TIMEOUT) {
					dprintk(KERN_DEBUG "SCXLNXSMCommPollingThread(%p) : prepare to sleep infinitely\n",
                     pSMComm);
				} else 		{
					dprintk(KERN_DEBUG "SCXLNXSMCommPollingThread(%p) : prepare to sleep 0x%lx jiffies\n",
                     pSMComm,
                     nRelativeTimeoutJiffies);
            }
            schedule_timeout(nRelativeTimeoutJiffies);
            dprintk(
						KERN_DEBUG "SCXLNXSMCommPollingThread(%p) : N_SM_EVENT signaled or timeout expired\n",
                  pSMComm);

            finish_wait(&pSMComm->waitQueue, &wait);
            goto begin;
         }
      }
      finish_wait(&pSMComm->waitQueue, &wait);
   }

#ifndef CONFIG_PREEMPT
		if (need_resched()) {
         schedule();
      }
#endif

end:
   SCXLNXSMCommSetCurrentTime(pSMComm);

   return 0;
}

/*----------------------------------------------------------------------------*/
/*
 * Sends the specified message through the specified SM communication.
 *
 * This function sends the message and returns immediately
 *
 * If pConn is not NULL, before sending the message, this function checks that it is still
 * valid by calling the function SCXLNXConnCheckMessageValidity
 *
 * Returns zero upon successful completion, or an appropriate error code upon
 * failure.
 */
static int SCXLNXSMCommSendMessage(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      SCX_COMMAND_MESSAGE* pMessage,
      SCXLNX_CONN_MONITOR* pConn,
      int bKillable)
{
   int nError;
   u32 nFirstFreeCommand;
   u32 nFirstCommand;

   dprintk(KERN_INFO "SCXLNXSMCommSendMessage(%p, %p)\n",
         pSMComm, pMessage);

   SCXLNXDumpMessage(pMessage);

   /*
    * Check if the current user space process
    * has received an interrupt.
    * If so, return immediately with correct error code
    */
	if (bKillable &&  (signal_pending (current))) {
      nError = -EINTR;
      goto error;
   }

	if (pConn != NULL && !SCXLNXConnCheckMessageValidity(pConn, pMessage)) {
      /* We must not send the message after all... */
      nError = -ENOTTY;
      goto error;
   }

   /*
    * Write the message in the message queue.
    */
retry:
   spin_lock(&pSMComm->lock);

   nFirstCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstCommand);
   nFirstFreeCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeCommand);

	if (nFirstFreeCommand - nFirstCommand >= 32) {
      /* Command queue is full Yield to the secure world and try again */
      spin_unlock(&pSMComm->lock);
		dprintk(KERN_DEBUG "SCXLNXSMCommSendMessage(%p) : wake up the polling thread (Command queue full)\n",
            pSMComm);
      wake_up(&(pSMComm->waitQueue));
#ifndef CONFIG_PREEMPT
		if (need_resched()) {
         schedule();
      }
#endif

      /*
       * Check if the current user space process
       * has received an interrupt.
       * If so, return immediately with correct error code
       */
		if (bKillable &&  (signal_pending (current))) {
         nError = -EINTR;
         goto error;
      }

      goto retry;
   }

   (void)memcpy(
      &pSMComm->pBuffer->sCommandQueue[nFirstFreeCommand%SCX_SM_N_MESSAGE_QUEUE_CAPACITY],
      pMessage, sizeof(SCX_COMMAND_MESSAGE));

   nFirstFreeCommand++;

   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->nFirstFreeCommand, nFirstFreeCommand);

   spin_unlock(&pSMComm->lock);

   /*
    * Yield the PA
    */
	dprintk(KERN_DEBUG "SCXLNXSMCommSendMessage(%p) : wake up the polling thread\n",
         pSMComm);
   wake_up(&(pSMComm->waitQueue));

   /*
    * Successful completion.
    */

	dprintk(KERN_INFO "SCXLNXSMCommSendMessage(%p) : Success\n", pSMComm);
   return 0;

   /*
    * Error handling.
    */

error:
	dprintk(KERN_ERR "SCXLNXSMCommSendMessage(%p) : Failure (error %d)\n",
         pSMComm, nError);
   return nError;
}

#ifdef SMC_CACHE_OPTIM
/*----------------------------------------------------------------------------*/
/*Flush the shared memories cache					*/
/*----------------------------------------------------------------------------*/

static void sclnx_device_flush_shared_mem_cache(SCXLNX_CONN_MONITOR *pConn)
{
	SCXLNX_SHMEM_DESC *pShmemDesc;
	SCXLNX_SHMEM_MONITOR *pShmemMonitor;
	pShmemMonitor = &(pConn->sSharedMemoryMonitor);
	down_interruptible(&(pShmemMonitor->sharedMemoriesMutex));
	list_for_each_entry(pShmemDesc, &(pShmemMonitor->sUsedSharedMemoryList), list)
	{
		v7_dma_flush_range(
				(u32)pShmemDesc->pBuffer,
				(u32)pShmemDesc->pBuffer + pShmemDesc->nAllocatedBufferSize);
	}
	up(&(pShmemMonitor->sharedMemoriesMutex));
}
#endif /*SMC_CACHE_OPTIM */
/* Sends the specified message through the specified SM communication.
 *
 * This function sends the message and waits for the corresponding answer
 * It may return if a signal needs to be delivered.
 *
 * If pConn is not NULL, before sending the message, this function checks that it is still
 * valid by calling the function SCXLNXConnCheckMessageValidity
 *
 * Returns zero upon successful completion, or an appropriate error code upon
 * failure.
 */
int SCXLNXSMCommSendReceive(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      SCX_COMMAND_MESSAGE *pMessage,
      SCX_ANSWER_MESSAGE *pAnswer,
      SCXLNX_CONN_MONITOR* pConn,
      int bKillable)
{
   int nError;
   SCXLNX_SM_ANSWER_STRUCT *pAnswerStructure = NULL;
   sigset_t nOldSet, nNewSet;

   dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive(%p) - message=%p answer=%p\n",
         pSMComm,
         pMessage,
         pAnswer);

	if (bKillable) {
		/*
      /*
       * only allow the SIGKILL signal to interrupt the operation
       */
      siginitsetinv(&nNewSet, sigmask(SIGKILL));
	} else {
		/*
      /*
       * do not allow any signal
       */
      siginitsetinv(&nNewSet, 0);
   }
   sigprocmask(SIG_BLOCK, &nNewSet, &nOldSet);

   pAnswerStructure = (SCXLNX_SM_ANSWER_STRUCT*) internal_kmalloc(sizeof(SCXLNX_SM_ANSWER_STRUCT), GFP_KERNEL);
	if (pAnswerStructure == NULL) {
      nError = -ENOMEM;
      goto error;
   }

   pAnswerStructure->pAnswer = pAnswer;
   pAnswerStructure->nSendingThreadAvailable = 1;
   pMessage->nOperationID = (u32) pAnswerStructure;

   init_completion(&pAnswerStructure->sAnswerEvent);

	if (pAnswer != NULL) {
      pAnswer->nSChannelStatus = S_PENDING;
   }

   /*
    * Send message if any.
    */


#ifdef SMC_CACHE_OPTIM
		switch (pMessage->nMessageType) {
		case SCX_MESSAGE_TYPE_OPEN_CLIENT_SESSION:
		case SCX_MESSAGE_TYPE_CLOSE_CLIENT_SESSION:
		case SCX_MESSAGE_TYPE_INVOKE_CLIENT_COMMAND:
		{
			sclnx_device_flush_shared_mem_cache(pConn);
		}
		break;
		default:
		break;
		}
#endif /*SMC_CACHE_OPTIM */
   /*
    * Send the command
    */

   nError = SCXLNXSMCommSendMessage(
         pSMComm,
         pMessage,
         pConn,
         bKillable);

	if (nError != 0) {
      dprintk(
				KERN_ERR "SCXLNXSMCommSendReceive(%p) : SCXLNXSMCommSendMessage failed (error %d) !\n",
            pSMComm, nError);
      goto error;
   }

   /* Now, wait for the answer */
	while (pAnswer != NULL) {
      nError = wait_for_completion_interruptible(&pAnswerStructure->sAnswerEvent);
		if (nError == -ERESTARTSYS) {
			/*
         /*
          * "wait for answer" operation failed, check why
          */
#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
		if (unlikely(freezing(current))) {
         powerPrintk(KERN_INFO "SCXLNXTZCommPollingThread: Entering refrigerator.\n");
         refrigerator();
         powerPrintk(KERN_INFO "SCXLNXTZCommPollingThread: Left refrigerator.\n");
      }
#endif

			if (bKillable && signal_pending(current)) {
				/*
            /*
             * The user space thread has received a signal
             * The only unblocked signal is SIGKILL
             * So the application should stop, return appropriate
             * error code.
             */

            nError = -EINTR;
				dprintk(KERN_INFO "SCXLNXSMCommSendReceive(%p) : Interrupted by user signal [%d]\n",
                     pSMComm, nError);

            /* Don't deallocate the answer structure yet, SCXLNXTZCommConsumeAnswers
               will do it (and will send a DESTROY_DEVICE_CONTEXT message as well) */
            pAnswerStructure->nSendingThreadAvailable = 0;

            /* restore signal maskss */
            sigprocmask(SIG_SETMASK, &nOldSet, NULL);

            return nError;
         }

         /*
          * ending up here means that wait_for_completion_interruptible()
          * has failed for an unknown reason. Loop again ?
          */
         continue;
      }

      /* the answer has been received, return */
      break;
   }


	if (pAnswer != NULL) {
		dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive(%p) : Message answer ready\n",
            pSMComm);

      /* print out a warning if the answer is not success */
		if (pAnswer->nSChannelStatus != S_SUCCESS) {
         dprintk(
					KERN_WARNING "SCXLNXSMCommSendReceive(%p) : Command failed with nSChannelStatus=0x%08x\n",
               pSMComm, pAnswer->nSChannelStatus);
         goto error;
      }
   }

   internal_kfree(pAnswerStructure);

   /* restore signal maskss */
   sigprocmask(SIG_SETMASK, &nOldSet, NULL);
   /* successful completion */
	dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive(%p) : Returns success\n",
         pSMComm);

   return 0;

error:
   /* restore signal maskss */
   sigprocmask(SIG_SETMASK, &nOldSet, NULL);

   internal_kfree(pAnswerStructure);

   return nError;
}

/*----------------------------------------------------------------------------*/

u32 SCXLNXSMCommRPCHandler(u32 nRPCId, u32 nRPCCommand, u32 nReserved1, u32 nReserved2)
{
   /*
    * Implementation note:
    * 1/ This routine is called in the context of the thread that has started
    *    the PA, i.e. SCXLNXSMCommPollingThread.
    * 2/ The L0 shared buffer is used to pass parameters from the PA SMC,
    *    and to return results to the PA SMC.
    */
   SCXLNX_SM_COMM_MONITOR*    pSMComm;

   u32 nRPCCall = RPC_SUCCESS;

	if (nRPCId != RPC_ID_SMODULE) {
      printk(KERN_ERR "SMC: RPC Handler: Invalid RPCId=0x%x, RPCCmd=0x%x [Ignored]\n",
               nRPCId, nRPCCommand);
      return RPC_ERROR_BAD_PARAMETERS;
   }


   pSMComm = g_pSMComm;

   BUG_ON(pSMComm == NULL);

	switch (nRPCCommand) {
   case RPC_CMD_YIELD:
      up(&(pSMComm->pubcrypto.HWALock));
      nRPCCall = SCXLNXSMCommPollingRoutine(pSMComm);
      down(&(pSMComm->pubcrypto.HWALock));
      g_L1SharedReady = true;
      break;

   case RPC_CMD_INIT:
      /*
       * Initialization phase in the normal world.
       * This is part of the PA initialization process in order to:
       *    > release the PA buffer (not required anymore)
       *    > rheck the protocol version (returned by the PA).
       */
      spin_lock(&(pSMComm->lock));
      nRPCCall = SCXLNXSMCommRPCInit(pSMComm);
      spin_unlock(&(pSMComm->lock));
      break;

   case RPC_CMD_TRACE:
      spin_lock(&(pSMComm->lock));
      secure_dprintk(KERN_INFO "%s\n", &(((SCHANNEL_L0_BUFFER_INPUT*)pSMComm->pL0SharedBuffer)->sReserved[RPC_TRACE_OFFSET]) );
      spin_unlock(&(pSMComm->lock));
      break;

   case RPC_CMD_PUBLIC_CRYPTO_INIT:
      nRPCCall = scxPublicCryptoRPCInit(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_ENABLE_CLOCK:
      nRPCCall = scxPublicCryptoRPCEnableClock(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_UPDATE:
      nRPCCall = scxPublicCryptoRPCUpdate(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_FINAL:
      nRPCCall = scxPublicCryptoRPCFinal(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_TERMINATE:
      nRPCCall = scxPublicCryptoRPCTerminate(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_INVALIDATE_SESSION:
      pSMComm->pubcrypto.pCurrentSession = NULL;
      break;


   default:
      nRPCCall = RPC_ERROR_BAD_PARAMETERS;
		dprintk(KERN_ERR "SCXLNXSMCommRPCHandler(%p) : Invalid RPCCommand [0x%x]\n",
               pSMComm, nRPCCommand);
      break;
   }

   /*
    * OMAP3430 Secure ROM Code Functional Specification:
    *    L2 Cache is not used by SW which runs in Secure Mode.
    *    Thus, the non-Secure World�s software must ensure that any data
    *    in L2 Cache are coherent with memory before feeding such data to the
    *    Secure World for processing.
    */
   spin_lock(&(pSMComm->lock));
#ifdef SMC_CACHE_OPTIM
	v7_dma_flush_range(
		  (u32)pSMComm->pL0SharedBuffer,
		  (u32)pSMComm->pL0SharedBuffer + PAGE_SIZE);
	v7_dma_flush_range(
		  (u32)pSMComm->pBuffer,
		  (u32)pSMComm->pBuffer + PAGE_SIZE);
#else
   v7_flush_kern_cache_all();
#endif /*SMC_CACHE_OPTIM */
   spin_unlock(&(pSMComm->lock));

   return nRPCCall;
}

/*----------------------------------------------------------------------------*/

void SCXLNXSMCommReturnFromIRQ(void)
{
   SCXLNX_SM_COMM_MONITOR* pSMComm = g_pSMComm;

	if ((pSMComm != NULL) && (g_L1SharedReady)) {
      SCXLNXSMCommConsumeAnswers(pSMComm);
#ifndef CONFIG_PREEMPT
      schedule();
#endif
      SCXLNXSMCommSetCurrentTime(pSMComm);
      /*
       * OMAP3430 Secure ROM Code Functional Specification:
       *    L2 Cache is not used by SW which runs in Secure Mode.
       *    Thus, the non-Secure World�s software must ensure that any data
       *    in L2 Cache are coherent with memory before feeding such data to the
       *    Secure World for processing.
       */
      spin_lock(&(pSMComm->lock));
#ifdef SMC_CACHE_OPTIM
		v7_dma_flush_range(
			  (u32)pSMComm->pL0SharedBuffer,
			  (u32)pSMComm->pL0SharedBuffer + PAGE_SIZE);
		v7_dma_flush_range(
			  (u32)pSMComm->pBuffer,
			  (u32)pSMComm->pBuffer + PAGE_SIZE);
#else
      v7_flush_kern_cache_all();
#endif /*SMC_CACHE_OPTIM */
      spin_unlock(&(pSMComm->lock));
	} else {
      /* Nothing to do actually */
   }
}



/*----------------------------------------------------------------------------
 * Power management
 *----------------------------------------------------------------------------*/

/*
 * Perform a shutdown operation.
 * The routine does not return if the operation succeeds.
 * the routine returns an appropriate error code if
 * the operation fails.
 */
static inline int SCXLNXSMCommShutdown(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   int nError;
   SCX_COMMAND_MESSAGE sMessage;

   dprintk(KERN_INFO "SCXLNXSMCommShutdown(%p)\n", pSMComm);

   set_bit(SCXLNX_SM_COMM_FLAG_TERMINATING, &(pSMComm->nFlags));

   sMessage.nMessageType = SCX_MESSAGE_TYPE_POWER_MANAGEMENT;
   sMessage.sBody.sPowerManagementMessage.nPowerCommand = SCPM_PREPARE_SHUTDOWN;
   sMessage.sBody.sPowerManagementMessage.nSharedMemDescriptors[0] = 0;
   sMessage.sBody.sPowerManagementMessage.nSharedMemDescriptors[1] = 0;
   sMessage.sBody.sPowerManagementMessage.nSharedMemSize = 0;
   sMessage.sBody.sPowerManagementMessage.nSharedMemStartOffset = 0;

   nError = SCXLNXSMCommSendReceive(
         pSMComm,
         &sMessage,
         NULL, /* No answer required */
         NULL,
         0);

	if (nError != 0) {
      dprintk(
				KERN_ERR "SCXLNXSMCommShutdown(%p) : SCXLNXSMCommSendReceive failed (error %d) !\n",
            pSMComm, nError);
      return nError;
   }

	dprintk(KERN_INFO "SCXLNXSMCommShutdown(%p) : Waiting for polling thread death...\n",
         pSMComm);
   wake_up(&(pSMComm->waitQueue));
   wait_for_completion(&(pSMComm->pollingThreadDeath));

	dprintk(KERN_INFO "SCXLNXSMCommShutdown(%p) : Success\n", pSMComm);

   return 0;
}


/*
 * Handles all the power management calls.
 * The nOperation is the type of power management
 * operation to be performed.
 */
int SCXLNXSMCommPowerManagement(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      SCXLNX_SM_POWER_OPERATION nOperation)
{
   u32 nStatus;
   int nError = 0;

	dprintk(KERN_INFO "SCXLNXSMCommPowerManagement(%p, %d)\n", pSMComm, nOperation);

	if ((test_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags))) == 0) {
		dprintk(KERN_INFO "SCXLNXSMCommPowerManagement(%p) : succeeded (not started)\n", pSMComm);
      return 0;
   }

   nStatus = ((SCXLNXSMCommReadReg32(&(pSMComm->pBuffer->nStatus_S))
         & SCX_SM_STATUS_POWER_STATE_MASK)
         >> SCX_SM_STATUS_POWER_STATE_SHIFT);

	switch (nOperation) {
   case SCXLNX_SM_POWER_OPERATION_SHUTDOWN:

		switch (nStatus) {
      case SCX_SM_POWER_MODE_ACTIVE:
         nError = SCXLNXSMCommShutdown(pSMComm);

			if (nError) {
				dprintk(KERN_ERR "SCXLNXSMCommPowerManagement(%p) : Failed with error code 0x%08x\n", pSMComm, nError);
            goto error;
         }
         break;

      default:
			dprintk(KERN_ERR "SCXLNXSMCommPowerManagement(%p) : Power command not allowed in current state %d\n", pSMComm, nStatus);
         nError = -ENOTTY;
         goto error;
      }
      break;


   default:
      nError = -ENOTSUPP;
		dprintk(KERN_ERR "SCXLNXSMCommPowerManagement(%p) : Operation not supported [%d]\n", pSMComm, nOperation);
      goto error;
   }

	dprintk(KERN_INFO "SCXLNXSMCommPowerManagement(%p) : succeeded\n", pSMComm);
   return 0;

error:
   return nError;
}


/*
 * Saves the context of the secure world
 */
#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
int SCXLNXCommSaveContext(uint32_t nPhysicalAddress)
{
   int nError;

   nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_CONTEXT_SAVE_RESTORE,
                                          0,
                                          FLAG_START_HAL_CRITICAL,
                                          4,
                                          nPhysicalAddress, /* Physical address in sdram where to save the secure ram (u8*) */
                                          0x1E, /* First Dma channel */
                                          0x1F, /* Second Dma channel */
                                          1     /* The direction. 1 for save, 0 for restore. */
                                        );

	if (nError != API_HAL_RET_VALUE_OK) {
      powerPrintk("Context save Error=%d PhysAddr=0x%x", nError, nPhysicalAddress);
      return -EFAULT;
   }

   powerPrintk("Context save OK PhysAddr=0x%x", nPhysicalAddress);
   return 0;
}
#endif


/*----------------------------------------------------------------------------
 * Communication initialization and termination
 *----------------------------------------------------------------------------*/

/*
 * Resets the communication descriptor
 */
void SCXLNXSMCommReset(
            SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   spin_lock_init(&(pSMComm->lock));
   pSMComm->nFlags = 0;
   pSMComm->pBuffer = NULL;
   pSMComm->pL0SharedBuffer = NULL;
   pSMComm->pPAInfo = NULL;
   pSMComm->pPABuffer = NULL;
   init_waitqueue_head(&(pSMComm->waitQueue));
   init_completion(&(pSMComm->pollingThreadDeath));

   pSMComm->bSDPInitialized = false;
   INIT_LIST_HEAD(&(pSMComm->pubcrypto.sessions));
   pSMComm->pubcrypto.nAesSession = 0;
   pSMComm->pubcrypto.nDesSession = 0;
   pSMComm->pubcrypto.nShaSession = 0;
}


int SCXLNXSMCommInit(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   u32 nError;

   dprintk(KERN_INFO "SCXLNXSMCommInit(%p)\n", pSMComm);

   SCXLNXSMCommReset(pSMComm);

#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
   init_timer(&pSMComm->pubcrypto.pPowerManagementTimer);
#endif /* SMODULE_SMC_OMAP3430_POWER_MANAGEMENT */

   nError = scxPublicCryptoInit(&(pSMComm->pubcrypto));
	if (nError != PUBLIC_CRYPTO_OPERATION_SUCCESS) {
      goto error;
   }


   g_pSMComm = pSMComm;


   return 0;

error:
   scxPublicCryptoTerminate(&(pSMComm->pubcrypto));
   return -EFAULT;
}


/*----------------------------------------------------------------------------*/
/*
 * Initializes the SE (SDP, SRAM size, RPC handler).
 *
 * Returns 0 upon success or appropriate error code
 * upon failure
 */
static int SCXLNXSMSDPInit(
      SCXLNX_SM_COMM_MONITOR* pSMComm,
      u32 nSDPBackingStoreAddr, u32 nSDPBkExtStoreAddr)
{
   int nError;
	unsigned int nCrc;

   dprintk(KERN_INFO "SCXLNXSMSDPInit\n");

	if (!pSMComm->bSDPInitialized) {
      /* Secure CRC Read  -------------------------------------------------------*/
      dprintk(KERN_INFO "SCXSMSDPInit: Secure CRC Read...\n");
		nCrc = SEC_ENTRY_pub2sec_dispatcher(API_HAL_KM_CRC_READ, 0, 0, 0, 0, 0, 0, 0);
		printk(KERN_INFO "SMC: SecureCRC=0x%08X\n", nCrc);

      /* SRAM RESIZE  -----------------------------------------------------------*/
      /* 60KB because the last 4KB are already used */
      dprintk(KERN_INFO "SCXLNXSMSDPInit: SRAM resize (60KB)...\n");
      nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_SEC_RAM_RESIZE,
                                             0,
                                             FLAG_FIQ_ENABLE|FLAG_START_HAL_CRITICAL,
                                             1,
															SEC_RAM_SIZE_60KB,
															0, 0, 0);
		if (nError == API_HAL_RET_VALUE_OK) {
         dprintk(KERN_INFO "SCXLNXSMSDPInit: SRAM resize OK\n");
		} else {
         dprintk(KERN_ERR "SCXLNXSMSDPInit: SRAM resize failed [0x%x]\n", nError);
         goto error;
      }

      /* SDP INIT ---------------------------------------------------------------*/
      dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP runtime init... (nSDPBackingStoreAddr=%x, nSDPBkExtStoreAddr=%x)\n",
                             nSDPBackingStoreAddr, nSDPBkExtStoreAddr);
      nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_SDP_RUNTIME_INIT,
                                             0,
                                             FLAG_FIQ_ENABLE|FLAG_START_HAL_CRITICAL,
                                             2,
                                             nSDPBackingStoreAddr,
															nSDPBkExtStoreAddr,
															0, 0);
		if (nError == API_HAL_RET_VALUE_SDP_RUNTIME_INIT_ERROR) {
			dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP runtime init already launched [0x%x]\n", nError);
		} else if (nError == API_HAL_RET_VALUE_OK) {
         dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP runtime init OK\n");
		} else {
         dprintk(KERN_ERR "SCXLNXSMSDPInit: SDP runtime init failed [0x%x]\n", nError);
         goto error;
      }

      /* RPC INIT  --------------------------------------------------------------*/
      dprintk(KERN_INFO "SCXLNXSMSDPInit: RPC init...\n");
      nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_SEC_RPC_INIT,
                                             0,
                                             FLAG_START_HAL_CRITICAL,
                                             1,
															(u32)(u32 (*const)(u32, u32, u32, u32)) &rpc_handler,
															0, 0, 0);
		if (nError == API_HAL_RET_VALUE_OK) {
         dprintk(KERN_INFO "SCXLNXSMSDPInit: RPC init OK\n");
		} else {
         dprintk(KERN_ERR "SCXLNXSMSDPInit: RPC init failed [0x%x]\n", nError);
         goto error;
      }

      pSMComm->bSDPInitialized = true;
	} else {
      dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP already initilized... nothing to do\n");
   }

   return 0;

error:
   return -EFAULT;
}


/*----------------------------------------------------------------------------*/
/*
 * Starts the SM.
 *
 * Returns 0 upon success or appropriate error code
 * upon failure
 */
int SCXLNXSMCommStart(
      SCXLNX_SM_COMM_MONITOR* pSMComm,
      u32 nSDPBackingStoreAddr, u32 nSDPBkExtStoreAddr,
      u8* pPABufferVAddr, u32 nPABufferSize,
      u8* pPropertiesBuffer, u32 nPropertiesBufferLength)
{
   SCHANNEL_C1S_BUFFER* pL1SharedBuffer = NULL;
   SCHANNEL_L0_BUFFER_INPUT* pL0SharedBuffer = NULL;
   SCHANNEL_L0_BUFFER_SMC_INIT_INPUT* pSMCInitInput;
   NS_PA_INFO*   paInfo = NULL;
   int  nError;

   /*
    * Implementation notes:
    *
    * 1/ The PA buffer (pPABufferVAddr) is now owned by this function.
    *    In case of error, it is responsible for releasing the buffer.
    *
    * 2/ The PA Info and PA Buffer will be freed through a RPC call
    *    at the beginning of the PA entry in the SE.
    */

   dprintk(KERN_INFO "SCXLNXSMCommStart(%p)\n", pSMComm);

	if ((test_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags))) != 0) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : The SM is already started\n", pSMComm);
      nError = -EFAULT;
      goto error1;
   }

	if ((sizeof(SCHANNEL_L0_BUFFER_INPUT) != PAGE_SIZE) || (sizeof(SCHANNEL_C1S_BUFFER) != PAGE_SIZE)) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : The L0 or L1 structure size is incorrect !\n", pSMComm);
      nError = -EFAULT;
      goto error1;
   }

   nError = SCXLNXSMSDPInit(pSMComm, nSDPBackingStoreAddr, nSDPBkExtStoreAddr);
	if (nError != 0) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : SDP init failed with %x!\n", pSMComm, nError);
      goto error1;
   }

   paInfo = (NS_PA_INFO*)internal_kmalloc(sizeof(NS_PA_INFO), GFP_KERNEL);

   pL0SharedBuffer = (SCHANNEL_L0_BUFFER_INPUT*)internal_get_zeroed_page(GFP_KERNEL);
   pL1SharedBuffer = (SCHANNEL_C1S_BUFFER*)internal_get_zeroed_page(GFP_KERNEL);

	if ((paInfo == NULL) || (pL0SharedBuffer == NULL) || (pL1SharedBuffer == NULL)) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : Out of memory\n", pSMComm);
      nError = -ENOMEM;
      goto error1;
   }

   /*
    * Ensure the page storing the SM communication buffer is mapped.
    */
#ifdef KERNEL_2_6_27
   set_page_locked(virt_to_page(pL0SharedBuffer));
   set_page_locked(virt_to_page(pL1SharedBuffer));
#else
   SetPageLocked(virt_to_page(pL0SharedBuffer));
   SetPageLocked(virt_to_page(pL1SharedBuffer));
#endif

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : L0SharedBuffer={0x%x, 0x%x}\n",
            pSMComm, (u32)pL0SharedBuffer, (u32)__pa(pL0SharedBuffer));

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : L1SharedBuffer={0x%x, 0x%x}\n",
            pSMComm, (u32)pL1SharedBuffer, (u32)__pa(pL1SharedBuffer));

   pSMComm->pPAInfo = paInfo;
   pSMComm->pPABuffer = pPABufferVAddr;
   pSMComm->pL0SharedBuffer = pL0SharedBuffer;
   pSMComm->pBuffer = pL1SharedBuffer;

   paInfo->pCertificate = (void*)__pa(pPABufferVAddr);
   paInfo->pParameters = (void*)__pa(pL0SharedBuffer);
   paInfo->pResults = (void*)__pa(pL0SharedBuffer);

   memset(pL0SharedBuffer, 0, sizeof(SCHANNEL_L0_BUFFER_INPUT));
   memset(pL1SharedBuffer, 0, sizeof(SCHANNEL_C1S_BUFFER));

   pSMCInitInput = (SCHANNEL_L0_BUFFER_SMC_INIT_INPUT*)pL0SharedBuffer;

   pSMCInitInput->nL1Command = SCX_SM_SMC_INIT;
   pSMCInitInput->nL1SharedBufferLength = sizeof(SCHANNEL_C1S_BUFFER);
   pSMCInitInput->nL1SharedBufferPhysAddr = __pa(pL1SharedBuffer);

	pSMCInitInput->nBackingStoreAddr   = nSDPBackingStoreAddr;
	pSMCInitInput->nBackExtStorageAddr = nSDPBkExtStoreAddr;
   pSMCInitInput->nPropertiesBufferLength = nPropertiesBufferLength;
	if (nPropertiesBufferLength == 0) {
      pSMCInitInput->pPropertiesBuffer[0] = 0;
	} else {
      memcpy(pSMCInitInput->pPropertiesBuffer, pPropertiesBuffer, nPropertiesBufferLength);
   }

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : System Configuration (%d bytes)\n",
            pSMComm, pSMCInitInput->nPropertiesBufferLength);

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : Starting PA (%d bytes)...\n",
            pSMComm, nPABufferSize);

   /*
    * Create the PA running thread.
    */

   nError = kernel_thread(SCXLNXSMCommPollingThread, pSMComm, CLONE_KERNEL);
	if (nError < 0) {
      dprintk(
				KERN_ERR "SCXLNXSMCommStart(%p) : Kernel_thread failed [%d] !\n",
            pSMComm, nError);
      goto error2;
   }

   set_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags));

   return 0;

error1:
   internal_kfree(pPABufferVAddr);
   internal_kfree(paInfo);
	if (pL0SharedBuffer != NULL) {
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pL0SharedBuffer));
#else
   ClearPageLocked(virt_to_page(pL0SharedBuffer));
#endif
      internal_free_page((unsigned long)pL0SharedBuffer);
   }
	if (pL1SharedBuffer != NULL) {
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pL1SharedBuffer));
#else
   ClearPageLocked(virt_to_page(pL1SharedBuffer));
#endif
      internal_free_page((unsigned long)pL1SharedBuffer);
   }

error2:
   /*
    * Error handling.
    */

	dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : Failure [%d]\n",
            pSMComm, nError);

   SCXLNXSMCommStop(pSMComm);

   return nError;
}

/*----------------------------------------------------------------------------*/

void SCXLNXSMCommStop(SCXLNX_SM_COMM_MONITOR* pSMComm)
{
   int  nError;

   spin_lock(&(pSMComm->lock));

   dprintk(KERN_INFO "SCXLNXSMCommStop(%p)\n", pSMComm);

   /* PA unload */
   nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_PA_UNLOAD_ALL,
                                          0,
                                          FLAG_START_HAL_CRITICAL,
														0, 0, 0, 0, 0);
	if (nError != API_HAL_RET_VALUE_OK) {
		dprintk(KERN_ERR "SCXLNXSMCommStop(%p) : SM Unload failed [0x%x]\n",
               pSMComm, nError);
   }

	if (pSMComm->pBuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : L1SharedBuffer released\n", pSMComm);
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pSMComm->pBuffer));
#else
   ClearPageLocked(virt_to_page(pSMComm->pBuffer));
#endif
      internal_free_page((unsigned long) pSMComm->pBuffer);
      pSMComm->pBuffer = NULL;
   }

	if (pSMComm->pL0SharedBuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : L0SharedBuffer released\n", pSMComm);
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pSMComm->pL0SharedBuffer));
#else
   ClearPageLocked(virt_to_page(pSMComm->pL0SharedBuffer));
#endif
      internal_free_page((unsigned long) pSMComm->pL0SharedBuffer);
      pSMComm->pL0SharedBuffer = NULL;
   }

	if (pSMComm->pPABuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : PA Buffer released\n", pSMComm);
      internal_kfree(pSMComm->pPABuffer);
      pSMComm->pPABuffer = NULL;
   }

	if (pSMComm->pPAInfo != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : PA Info released\n", pSMComm);
      internal_kfree(pSMComm->pPAInfo);
      pSMComm->pPAInfo = NULL;
   }

   spin_unlock(&(pSMComm->lock));
}

/*----------------------------------------------------------------------------*/

/*
 * Attempt to terminate the communication.
 */
void SCXLNXSMCommTerminate(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   dprintk(KERN_INFO "SCXLNXSMCommTerminate(%p)\n", pSMComm);

   spin_lock(&(pSMComm->lock));

   scxPublicCryptoTerminate(&(pSMComm->pubcrypto));

   g_pSMComm = NULL;

   spin_unlock(&(pSMComm->lock));
}

/*
 * Copyright (c) 2006-2008 Trusted Logic S.A.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <asm/div64.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/interrupt.h>
#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/jiffies.h>

#include "scxlnx_defs.h"
#include "scxlnx_sm_comm.h"
#include "scxlnx_util.h"
#include "scxlnx_conn.h"

#include "scx_public_crypto.h"

#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
#include <linux/freezer.h>
#endif


#define SECURE_TRACE

#ifdef SECURE_TRACE
#define secure_dprintk  printk
#else
#define secure_dprintk(args...)  do { ; } while (0)
#endif /* defined (SECURE_TRACE) */


/*---------------------------------------------------------------------------
 * Internal Constants
 *---------------------------------------------------------------------------*/

/*
 * Trace offset
 */
#define RPC_TRACE_OFFSET 0xC00 // traces start at offset 3072


/*
 * RPC Identifiers
 */
#define RPC_ID_SEC_STORAGE    0x00
#define RPC_ID_DMA            0x01
#define RPC_ID_SMODULE        0x02

/*
 * RPC Return values
 */
#define RPC_SUCCESS                    0x00000000
#define RPC_ERROR_GENERIC              0xFFFF0000
#define RPC_ERROR_BAD_PARAMETERS       0xFFFF0006
#define RPC_ERROR_OUT_OF_MEMORY        0xFFFF000C
#define RPC_ERROR_CONNECTION_PROTOCOL  0xFFFF3020

/*
 * RPC Commands
 */
#define RPC_CMD_YIELD                        0x00
#define RPC_CMD_INIT                         0x01
#define RPC_CMD_TRACE                        0x02

#define RPC_CMD_PUBLIC_CRYPTO_INIT               0x10
#define RPC_CMD_PUBLIC_CRYPTO_UPDATE             0x11
#define RPC_CMD_PUBLIC_CRYPTO_FINAL              0x12
#define RPC_CMD_PUBLIC_CRYPTO_TERMINATE          0x13
#define RPC_CMD_PUBLIC_CRYPTO_ENABLE_CLOCK       0x14
#define RPC_CMD_PUBLIC_CRYPTO_INVALIDATE_SESSION 0x15


/*
 * SE entry flags
 */
#define FLAG_START_HAL_CRITICAL     0x4
#define FLAG_IRQFIQ_MASK            0x3
#define FLAG_IRQ_ENABLE             0x2
#define FLAG_FIQ_ENABLE             0x1

/*
 * HAL API Identifiers
 */
#define API_HAL_PA_LOAD             15
#define API_HAL_PA_UNLOAD_ALL       17
#define API_HAL_SDP_RUNTIME_INIT    19
#define API_HAL_SEC_RPC_INIT        21
#define API_HAL_SEC_RAM_RESIZE      26
#define API_HAL_CONTEXT_SAVE_RESTORE  25
#define API_HAL_KM_CRC_READ               34


/*
 * HAL API return codes
 */
#define API_HAL_RET_VALUE_OK        0x0
#define API_HAL_RET_VALUE_SDP_RUNTIME_INIT_ERROR        0x20

/*
 * HAL API RAM Resize values
 */
#define SEC_RAM_SIZE_48KB     0x0000C000
#define SEC_RAM_SIZE_60KB     0x0000F000
#define SEC_RAM_SIZE_64KB     0x00010000


/*
 * Time constants
 */
#define TIME_IMMEDIATE ((u64) 0x0000000000000000ULL)
#define TIME_INFINITE  ((u64) 0xFFFFFFFFFFFFFFFFULL)

/*
 * The name of the polling thread.
 */
#define SCXLNX_SM_COMM_POLLING_THREAD_NAME      SCXLNX_DEVICE_BASE_NAME

/*
 * The nOperationID field of a message points to this structure.
 * It is used to identify the thread that triggered the message transmission
 * Whoever reads an answer can wake up that thread using the completion event
 */
typedef struct {
   struct completion sAnswerEvent;
   SCX_ANSWER_MESSAGE* pAnswer;

   u32 nSendingThreadAvailable;
} SCXLNX_SM_ANSWER_STRUCT;


typedef struct {
   void  *pCertificate;
   void  *pParameters;
   void  *pResults;

} NS_PA_INFO;


static SCXLNX_SM_COMM_MONITOR* g_pSMComm = NULL;
static bool g_L1SharedReady = false;



/*---------------------------------------------------------------------------
 * Extern declarations
 *---------------------------------------------------------------------------*/

extern u32 pub2sec_bridge_entry(u32 appl_id, u32 proc_ID, u32 flag, u32 paNextArgs);
extern u32 rpc_handler(u32 p1, u32 p2, u32 p3, u32 p4);
extern u32 v7_flush_kern_cache_all(void);
#ifdef SMC_CACHE_OPTIM
extern u32 v7_dma_flush_range(u32 nVAStart, u32 nVAEnd);
#endif /* SMC_CACHE_OPTIM */

/*----------------------------------------------------------------------------
 * Function responsible for formatting the parameters to pass
 * from NS-World to S-World.
 *----------------------------------------------------------------------------*/
u32 SEC_ENTRY_pub2sec_dispatcher(u32 appl_id, u32 proc_ID, u32 flag, u32 nArgs,
											u32 arg1, u32 arg2, u32 arg3, u32 arg4)
{
   u32 return_value = 0;
   u32* pArgs = NULL;

	dprintk(KERN_INFO "SEC_ENTRY_pub2sec_dispatcher: ApplId=0x%x, ProcId=0x%x, flag=0x%x, args=%u\n",
				appl_id, proc_ID, flag, nArgs);

   /*
    * We need a physically contiguous buffer to pass parameters to the SE
    */
   pArgs = (u32*)internal_kmalloc(sizeof(u32) * 5, GFP_KERNEL);
	if (pArgs == NULL) {
      return -ENOMEM;
   }

	pArgs[0] = nArgs;
	pArgs[1] = arg1;
	pArgs[2] = arg2;
	pArgs[3] = arg3;
	pArgs[4] = arg4;

	dprintk(KERN_INFO "SEC_ENTRY_pub2sec_dispatcher: args=0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				pArgs[0], pArgs[1], pArgs[2], pArgs[3], pArgs[4]);

   /*
    * OMAP3430 Secure ROM Code Functional Specification:
    *    L2 Cache is not used by SW which runs in Secure Mode.
    *    Thus, the non-Secure World�s software must ensure that any data
    *    in L2 Cache are coherent with memory before feeding such data to the
    *    Secure World for processing.
    */
   v7_flush_kern_cache_all();

	return_value = pub2sec_bridge_entry(appl_id, proc_ID, flag, __pa(pArgs));

   internal_kfree(pArgs);

   return return_value;
}


/*---------------------------------------------------------------------------
 * atomic operation definitions
 *---------------------------------------------------------------------------*/

/*
 * Atomically reads the value of the 32-bit register in the SM
 * communication buffer, taking endianness issues into account.
 */
static inline u32 SCXLNXSMCommReadReg32(
      const u32 *pValue)
{
   return *(const volatile u32 *)pValue;
}


/*
 * Atomically overwrites the value of the 32-bit register at nOffset in the SM
 * communication buffer pointed to by pCommBuffer with nValue, taking endianness
 * issues into accout.
 */
static inline void SCXLNXSMCommWriteReg32(
      void *pCommBuffer,
      u32 nValue)
{
   *((volatile u32 *)pCommBuffer) = nValue;
}


/*----------------------------------------------------------------------------*/
/*
 * Free the PA buffers.
 * Check the protocol version (returned by the PA).
 */
static u32 SCXLNXSMCommRPCInit(SCXLNX_SM_COMM_MONITOR* pSMComm)
{
   u32 nProtocolVersion;
   u32 nRPCError = RPC_SUCCESS;

   dprintk(KERN_INFO "SCXLNXSMCommRPCInit(%p)\n", pSMComm);

	if (pSMComm->pPABuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommRPCInit(%p) : PA Buffer released\n", pSMComm);
      internal_kfree(pSMComm->pPABuffer);
      pSMComm->pPABuffer = NULL;
   }

	if (pSMComm->pPAInfo != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommRPCInit(%p) : PA Info released\n", pSMComm);
      internal_kfree(pSMComm->pPAInfo);
      pSMComm->pPAInfo = NULL;
   }

   nProtocolVersion = *((u32*)(pSMComm->pL0SharedBuffer));

	if ((GET_PROTOCOL_MAJOR_VERSION(nProtocolVersion)) != SCX_SM_S_PROTOCOL_MAJOR_VERSION) {
      printk(KERN_ERR "SMC: Unsupported SMC Protocol PA Major Version (0x%02X, Expected 0x%02X) !\n",
            GET_PROTOCOL_MAJOR_VERSION(nProtocolVersion),
            GET_PROTOCOL_MAJOR_VERSION(SCX_SM_S_PROTOCOL_MAJOR_VERSION));
      nRPCError = RPC_ERROR_CONNECTION_PROTOCOL;
	} else {
      nRPCError = RPC_SUCCESS;
   }

   return nRPCError;
}




/*----------------------------------------------------------------------------
 * SMC related operations
 *----------------------------------------------------------------------------*/

/*
 * Atomically updates the nSyncSerial_N and sTime_N register
 * nSyncSerial_N and sTime_N modifications are thread safe
 */
static inline void SCXLNXSMCommSetCurrentTime(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   u32 nNewSyncSerial;
   struct timeval now;
   u64 sTime64;

   /*
    * lock the structure while updating the L1 shared memory fields
    */
   spin_lock(&(pSMComm->lock));

   /* read nSyncSerial_N and change the TimeSlot bit field */
   nNewSyncSerial = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nSyncSerial_N) + 1;

   do_gettimeofday(&now);
   sTime64 = now.tv_sec;
   sTime64 = (sTime64 * 1000) + (now.tv_usec / 1000);

   /* Write the new sTime and nSyncSerial into shared memory */
   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->sTime_N[nNewSyncSerial & SCX_SM_SYNC_SERIAL_TIMESLOT_N].nTime[0], (u32) (sTime64 & 0xFFFFFFFF));
   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->sTime_N[nNewSyncSerial & SCX_SM_SYNC_SERIAL_TIMESLOT_N].nTime[1], (u32) (sTime64 >> 32));
   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->nSyncSerial_N, nNewSyncSerial);

   spin_unlock(&(pSMComm->lock));
}

/*----------------------------------------------------------------------------*/
/*
 * Performs the specific read timeout operation
 * The difficulty here is to read atomically 2 u32
 * values from the L1 shared buffer.
 * This is guaranteed by reading before and after the operation
 * the timeslot given by the SM
 */
static inline void SCXLNXSMCommReadTimeout(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      u64 *pTime)
{
   u32 nSyncSerial_S_initial = 0;
   u32 nSyncSerial_S_final = 1;
   u64 sTime;

   spin_lock(&pSMComm->lock);

	while (nSyncSerial_S_initial != nSyncSerial_S_final) {
      nSyncSerial_S_initial = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nSyncSerial_S);
      sTime = (u64) SCXLNXSMCommReadReg32(&pSMComm->pBuffer->sTimeout_S[nSyncSerial_S_initial&1].nTime[1]);
      sTime = (sTime << 32) +
            (u64) SCXLNXSMCommReadReg32(&pSMComm->pBuffer->sTimeout_S[nSyncSerial_S_initial&1].nTime[0]);

      nSyncSerial_S_final = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nSyncSerial_S);
   }

   spin_unlock(&pSMComm->lock);

   *pTime = sTime;
}


/*
 * Unlocks the physical memory pages
 * and frees the pages that need to
 */
void SCXLNXSMCommReleaseDescriptor(
      SCXLNX_SHMEM_DESC *pShmemDesc,
      u32 nFullCleanup)
{
	switch (pShmemDesc->nAllocType) {
      case SCXLNX_SHMEM_ALLOC_TYPE_NONE:
         /* Nothing to do */
         break;

      case SCXLNX_SHMEM_ALLOC_TYPE_REGISTER:
		if (pShmemDesc->ppPages != NULL) {
            u32 i;

			for (i = 0; i < pShmemDesc->nAllocatedBufferSize; i++) {
				if (nFullCleanup && !PageReserved(pShmemDesc->ppPages[i])) {
                  SetPageDirty(pShmemDesc->ppPages[i]);
               }
               internal_page_cache_release(pShmemDesc->ppPages[i]);
            }

            internal_kfree(pShmemDesc->ppPages);
            pShmemDesc->ppPages = NULL;
         }
         break;

      case SCXLNX_SHMEM_ALLOC_TYPE_KMALLOC:
		if (pShmemDesc->pAllocatedBuffer != NULL) {
            internal_kfree(pShmemDesc->pAllocatedBuffer);
            pShmemDesc->pAllocatedBuffer = NULL;
         }
         break;

      case SCXLNX_SHMEM_ALLOC_TYPE_PAGES:
		if (pShmemDesc->pAllocatedBuffer != NULL) {
            u32 nOffset;

            /* Mark the pages as unreserved */
			for (nOffset = 0; nOffset < pShmemDesc->nAllocatedBufferSize; nOffset += PAGE_SIZE) {
#ifdef KERNEL_2_6_27
               clear_page_locked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#else
               ClearPageLocked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#endif
            }

            internal_free_pages((unsigned long)pShmemDesc->pAllocatedBuffer, (unsigned long)pShmemDesc->nBufferOrder);
            pShmemDesc->pAllocatedBuffer = NULL;
         }
         break;
   }

   pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_NONE;
}


/*
 * Make sure the pages are allocated. If not allocated, do it
 * Locks down the physical memory pages
 * Verifies the memory attributes depending on pFlags
 */
int SCXLNXSMCommFillDescriptor(
      SCXLNX_SHMEM_DESC *pShmemDesc,
      u32 *pnBufferAddr,
      u32 nBufferSize,
      u32 *pnFlags)
{
   int nError = 0;

   if (  (*pnBufferAddr != 0)
       /* In this case (direct access): - try to access the memory directly
                                        - otherwise allocate a new buffer to avoid fork issues */
	&& (((*pnFlags) & (SCX_SHMEM_TYPE_DIRECT | SCX_SHMEM_TYPE_DIRECT_FORCE)) != 0)) {
		/*
      /*
       * This is a buffer registration attempt...
       */
      unsigned long nBufferVAddr = (unsigned long)(*pnBufferAddr);
      unsigned long end = (nBufferVAddr + nBufferSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
      unsigned long start = nBufferVAddr >> PAGE_SHIFT;
      int nPagesMapped, i;
      unsigned long nPhysAddr;
      unsigned long nPhysAddrTmp;
      u32 nBufferOffset;

      pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_REGISTER;

      /* Number of pages */
      pShmemDesc->nAllocatedBufferSize = end - start;
      pShmemDesc->ppPages = internal_kmalloc(
            pShmemDesc->nAllocatedBufferSize * sizeof(*(pShmemDesc->ppPages)), GFP_KERNEL);
		if (pShmemDesc->ppPages == NULL) {
         nError = -ENOMEM;
         goto next;
      }

		dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Registering 0x%x: %u pages\n",
            pShmemDesc,
            (u32)nBufferVAddr,
            pShmemDesc->nAllocatedBufferSize);

      down_read(&(current->mm->mmap_sem));
      nPagesMapped = internal_get_user_pages(
            current,
            current->mm,
            nBufferVAddr,
            pShmemDesc->nAllocatedBufferSize,
            1, /* write */
            0, /* force */
            pShmemDesc->ppPages,
            NULL);
      up_read(&(current->mm->mmap_sem));

		if (nPagesMapped < 0) {
			dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : get_user_pages failed [%d] !\n",
                  pShmemDesc, nPagesMapped);
         /* This is done to release properly the already mapped pages (i.e. none) */
         pShmemDesc->nAllocatedBufferSize = 0;
         nError = nPagesMapped;
         goto next;
      }

		if ((u32)nPagesMapped != pShmemDesc->nAllocatedBufferSize) {
			dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : get_user_pages got %d pages from %u !\n",
                  pShmemDesc, nPagesMapped, pShmemDesc->nAllocatedBufferSize);
         /* This is done to release properly the already mapped pages */
         pShmemDesc->nAllocatedBufferSize = (u32)nPagesMapped;
         nError = -EFAULT;
         goto next;
      }

      /* Check that the buffer is physically contiguous */
      nPhysAddr = page_to_phys(pShmemDesc->ppPages[0]);
      start = nPhysAddr;

		dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Page  0: PhysAddr=0x%x\n",
            pShmemDesc, (u32)nPhysAddr);

		for (i = 1; i < nPagesMapped; i++) {
         start += PAGE_SIZE;
         nPhysAddrTmp = page_to_phys(pShmemDesc->ppPages[i]);

			dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Page %2d: PhysAddr=0x%x\n",
               pShmemDesc, i, (u32)nPhysAddrTmp);

			if (start != nPhysAddrTmp) {
            nError = -EINVAL;
				dprintk(KERN_WARNING "SCXLNXSMCommFillDescriptor(%p) : Buffer not physically contiguous !\n",
                     pShmemDesc);
            goto next;
         }
      }

      nBufferOffset = (u32)(nBufferVAddr & ~PAGE_MASK);

      pShmemDesc->pBuffer = (u8*)(page_address(pShmemDesc->ppPages[0])) + nBufferOffset;
      pShmemDesc->nBufferSize = nBufferSize;

		dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Registered Mapped Addr=0x%x (Offset=%u)\n",
            pShmemDesc, (u32)pShmemDesc->pBuffer, nBufferOffset);

      /* Update the buffer address with the physical address */
      *pnBufferAddr = (u32)nPhysAddr + nBufferOffset;

      return 0;
   }

next:
	if (nError != 0) {
      SCXLNXSMCommReleaseDescriptor(pShmemDesc, 0);

      /*
       * Registration failed so go on with the allocation method.
       */
		dprintk(KERN_WARNING "SCXLNXSMCommFillDescriptor(%p) : Register failed -> Allocation\n",
            pShmemDesc);

      *pnBufferAddr = 0;
   }

	if (*pnBufferAddr == 0) {
      pShmemDesc->nBufferSize = nBufferSize;

      /* Default allocation method is kmalloc */
      pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_KMALLOC;

      /* Works out the best allocation function */
		if (pShmemDesc->nBufferSize <= 4 * PAGE_SIZE) {
         pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_PAGES;
		} else {
         u32 nOrder;

			for (nOrder = 0; nOrder < 16; nOrder++) {
				if (pShmemDesc->nBufferSize == (0x1 << nOrder)*PAGE_SIZE) {
               pShmemDesc->nAllocType = SCXLNX_SHMEM_ALLOC_TYPE_PAGES;
               break;
            }
         }
      }

		switch (pShmemDesc->nAllocType) {
         case SCXLNX_SHMEM_ALLOC_TYPE_KMALLOC:
            /* Will be rounded up to a page boundary */
            pShmemDesc->nAllocatedBufferSize = pShmemDesc->nBufferSize + PAGE_SIZE;
            pShmemDesc->pAllocatedBuffer = internal_kmalloc(
                  pShmemDesc->nAllocatedBufferSize, GFP_KERNEL);
			if (pShmemDesc->pAllocatedBuffer == NULL) {
               nError = -ENOMEM;
				dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : Out of memory for buffer (%u bytes)\n",
                        pShmemDesc, pShmemDesc->nAllocatedBufferSize);
               goto error;
            }

            /* Round it up to the page bondary */
            pShmemDesc->pBuffer = (u8*)PAGE_ALIGN((unsigned long)pShmemDesc->pAllocatedBuffer);
            pShmemDesc->nBufferPhysAddr = virt_to_phys(pShmemDesc->pBuffer);

            /*
             * __GFP_ZERO is not allowed in the allocation method,
             * so do it manually...
             */
            memset(pShmemDesc->pBuffer, 0, pShmemDesc->nBufferSize);

			dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : kmalloc: Size=%u, Alloc={0x%x->0x%x}\n",
                  pShmemDesc,
                  pShmemDesc->nAllocatedBufferSize,
                  (u32)pShmemDesc->pAllocatedBuffer,
                  (u32)pShmemDesc->pBuffer);
            break;

         case SCXLNX_SHMEM_ALLOC_TYPE_PAGES:
         {
            u32 nOffset;

            pShmemDesc->nBufferOrder = 0;
            pShmemDesc->nAllocatedBufferSize = (0x1 << pShmemDesc->nBufferOrder) * PAGE_SIZE;

			while (pShmemDesc->nAllocatedBufferSize < pShmemDesc->nBufferSize) {
               pShmemDesc->nBufferOrder ++;
               pShmemDesc->nAllocatedBufferSize = (0x1 << pShmemDesc->nBufferOrder) * PAGE_SIZE;
            }

            pShmemDesc->pAllocatedBuffer = (u8*)internal_get_free_pages(
                  GFP_KERNEL | __GFP_ZERO, pShmemDesc->nBufferOrder);
			if (pShmemDesc->pAllocatedBuffer == NULL) {
               nError = -ENOMEM;
				dprintk(KERN_ERR "SCXLNXSMCommFillDescriptor(%p) : Out of memory for buffer (%u bytes, order=%u)\n",
                        pShmemDesc,
                        pShmemDesc->nAllocatedBufferSize,
                        pShmemDesc->nBufferOrder);
               goto error;
            }

            /* Mark the pages as reserved */
			for (nOffset = 0; nOffset < pShmemDesc->nBufferSize; nOffset += PAGE_SIZE) {
#ifdef KERNEL_2_6_27
               set_page_locked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#else
               SetPageLocked(virt_to_page(((unsigned long)pShmemDesc->pAllocatedBuffer) + nOffset));
#endif
            }
            pShmemDesc->pBuffer = pShmemDesc->pAllocatedBuffer;
            pShmemDesc->nBufferPhysAddr = virt_to_phys(pShmemDesc->pBuffer);

			dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : get_free_pages: Size=%u, Order=%u, Alloc=0x%x\n",
                  pShmemDesc,
                  pShmemDesc->nAllocatedBufferSize,
                  pShmemDesc->nBufferOrder,
                  (u32)pShmemDesc->pAllocatedBuffer);
            break;
         }

         default:
            BUG_ON(true);  /* To make the compiler happy */
            break;
      }
   }

   /* Update the buffer address with the physical address */
   *pnBufferAddr = pShmemDesc->nBufferPhysAddr;

	dprintk(KERN_INFO "SCXLNXSMCommFillDescriptor(%p) : Success: PhysAddr=0x%x\n",
         pShmemDesc, *pnBufferAddr);

   return 0;

error:
   SCXLNXSMCommReleaseDescriptor(pShmemDesc, 0);
   return nError;
}


/*----------------------------------------------------------------------------
 * Standard communication operations
 *----------------------------------------------------------------------------*/

/*
 * Returns a non-zero value if the specified S-timeout has expired, zero
 * otherwise.
 *
 * The placeholder referenced to by pnRelativeTimeoutJiffies gives the relative timeout
 * from now in jiffies. It is set to zero if the S-timeout has expired, or to MAX_SCHEDULE_TIMEOUT
 * if the S-timeout is infinite.
 */
static int SCXLNXSMCommTestSTimeout(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      u64 sTimeout,
      signed long *pnRelativeTimeoutJiffies)
{
   struct timeval now;
   u64 sTime64;

   *pnRelativeTimeoutJiffies = 0;

   /* immediate timeout */
	if (sTimeout == TIME_IMMEDIATE) {
      return 1;
   }

   /* infinite timeout */
	if (sTimeout == TIME_INFINITE) {
      dprintk(KERN_DEBUG "SCXLNXSMCommTestSTimeout: timeout is infinite\n");
      *pnRelativeTimeoutJiffies = MAX_SCHEDULE_TIMEOUT;
      return 0;
   }

   do_gettimeofday(&now);
   sTime64 = now.tv_sec;
   /* will not overflow as operations are done on 64bit values */
   sTime64 = (sTime64 * 1000) + (now.tv_usec / 1000);

   /* timeout expired */
	if (sTime64 >= sTimeout) {
      dprintk(KERN_DEBUG "SCXLNXSMCommTestSTimeout: timeout expired\n");
      return 1;
   }

   /*
    * finite timeout, compute pnRelativeTimeoutJiffies
    */
   /* will not overflow as sTime64 < sTimeout */
   sTimeout -= sTime64;

   /* guaranty *pnRelativeTimeoutJiffies is a valid timeout */
	if ((sTimeout >> 32) != 0) {
      *pnRelativeTimeoutJiffies = MAX_JIFFY_OFFSET;
	} else {
      *pnRelativeTimeoutJiffies = msecs_to_jiffies((unsigned int) sTimeout);
   }

   dprintk(KERN_DEBUG "SCXLNXSMCommTestSTimeout: timeout is 0x%lx\n", *pnRelativeTimeoutJiffies);
   return 0;
}

/* Forward Declaration */
static int SCXLNXSMCommSendMessage(
      SCXLNX_SM_COMM_MONITOR *pTZComm,
      SCX_COMMAND_MESSAGE* pMessage,
      SCXLNX_CONN_MONITOR* pConn,
      int bKillable);

/*----------------------------------------------------------------------------*/
/*
 * Reads out all the available answers sent by the SModule.
 * For each answer read, wakes up the corresponding emitting thread
 * by using the nOperationID
 */
static void SCXLNXSMCommConsumeAnswers(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   u32 nFirstAnswer;
   u32 nFirstFreeAnswer;

start:
   spin_lock(&(pSMComm->lock));

   nFirstFreeAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeAnswer);
   nFirstAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstAnswer);

	if (nFirstAnswer != nFirstFreeAnswer) {
		do {
         SCX_ANSWER_MESSAGE* pComAnswer;
         SCXLNX_SM_ANSWER_STRUCT* pAnswerStructure;

         pComAnswer = &pSMComm->pBuffer->sAnswerQueue[nFirstAnswer%32];
         pAnswerStructure = (SCXLNX_SM_ANSWER_STRUCT*)pComAnswer->nOperationID;

         SCXLNXDumpAnswer(pComAnswer);

         (void)memcpy(
            pAnswerStructure->pAnswer,
            pComAnswer,
            sizeof(*pComAnswer));

         nFirstAnswer++;
         SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->nFirstAnswer, nFirstAnswer);

			if (pAnswerStructure->nSendingThreadAvailable == 1) {
            complete(&pAnswerStructure->sAnswerEvent);
			} else 	{
				/*
            /*
             * The sending thread has received the SIGKILL signal,
             * clean up structure ourselves
             */
            dprintk(KERN_INFO "SCXLNXTZCommConsumeAnswers : sending thread killed\n");

            /*
             * We need to record the successfull creation of a device context
             */
				if ((pAnswerStructure->pAnswer->nMessageType == SCX_MESSAGE_TYPE_CREATE_DEVICE_CONTEXT) && (pAnswerStructure->pAnswer->nSChannelStatus == S_SUCCESS)) {
               SCX_COMMAND_MESSAGE sMessage;
               int nError;
               SCXLNX_SM_ANSWER_STRUCT* pAnswerStructureDC = pAnswerStructure;

               spin_unlock(&(pSMComm->lock));

               dprintk(KERN_INFO "SCXLNXSMCommConsumeAnswers(%p) : sending DESTROY_CONTEXT\n", pSMComm);

               pAnswerStructureDC->nSendingThreadAvailable = 0;
               sMessage.nMessageType = SCX_MESSAGE_TYPE_DESTROY_DEVICE_CONTEXT;
               sMessage.nOperationID = (u32)pAnswerStructureDC;
               sMessage.sBody.sDestroyDeviceContextMessage.hDeviceContext
                  = pAnswerStructureDC->pAnswer->sBody.sCreateDeviceContextAnswer.hDeviceContext;

               nError = SCXLNXSMCommSendMessage(
                           pSMComm,
                           &sMessage,
                           NULL,
                           0);
               /* SCXLNXSMCommSendMessage cannot return an error because
                  it's not killable and not within a connection */
               BUG_ON(nError != 0);

               /* need to complete the job */
               goto start;
            }

            internal_kfree(pAnswerStructure);
         }
		} while (nFirstAnswer != nFirstFreeAnswer);
   }

   spin_unlock(&(pSMComm->lock));
}

/*----------------------------------------------------------------------------*/
/*
 * Implements the entry-point of the SM polling threads.
 */
static int SCXLNXSMCommPollingThread(
      void *pParam)
{
   /*
    * Implementation note:
    *    The paInfo will be freed through a RPC call at the beginning
    *    of the PA entry in the SE.
    */
   int  nError;
   SCXLNX_SM_COMM_MONITOR *pSMComm = (SCXLNX_SM_COMM_MONITOR *)pParam;

	dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : Starting\n", pSMComm);

   /* Call daemonize() to remove any user space mem maps and signal handlers */
   daemonize(SCXLNX_SM_COMM_POLLING_THREAD_NAME);

   /* PA call */
   nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_PA_LOAD,
                                          0,
                                          FLAG_IRQ_ENABLE|FLAG_FIQ_ENABLE|FLAG_START_HAL_CRITICAL,
                                          1,
														__pa(pSMComm->pPAInfo),
														0, 0, 0);
	if (nError == API_HAL_RET_VALUE_OK) {
      nError = ((SCHANNEL_L0_BUFFER_OUTPUT*)(pSMComm->pL0SharedBuffer))->nL1Status;
      if (nError == S_ERROR_SDP_RUNTIME_INIT_ADDR_CHECK_FAIL) {
			printk(KERN_ERR "SMC: BackingStore and BackExtStorage addresses differs between initialization and configuration file.\n");
		} else if (nError == 0) {
			dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : SMC PA ended successfully\n",
               pSMComm);
		} else {
         printk(KERN_WARNING "SMC: PA ended with an error [0x%X]\n", nError);
      }
	} else {
      printk(KERN_ERR "SMC: PA load failed [0x%X]\n", nError);
   }

   test_and_clear_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags));
   test_and_clear_bit(SCXLNX_SM_COMM_FLAG_TERMINATING, &(pSMComm->nFlags));

   SCXLNXSMCommStop(pSMComm);

	dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : Signaling polling thread death\n",
            pSMComm);
   complete(&(pSMComm->pollingThreadDeath));

	dprintk(KERN_INFO "SCXLNXSMCommPollingThread(%p) : End\n", pSMComm);
   return nError;
}

/*----------------------------------------------------------------------------*/

/*
 * Implements the SM polling routine.
 */
static u32 SCXLNXSMCommPollingRoutine(SCXLNX_SM_COMM_MONITOR* pSMComm)
{
   DEFINE_WAIT(wait);
   u64 sTimeout;
   signed long nRelativeTimeoutJiffies;

begin:
   {
      /*
       * Check that the SM communication is still alive.
       */

		if (test_bit(SCXLNX_SM_COMM_FLAG_TERMINATING, &(pSMComm->nFlags)) != 0) {
			dprintk(KERN_INFO "SCXLNXSMCommPollingRoutine(%p) : Flag Terminating is set\n",
                  pSMComm);
         goto end;
      }

      prepare_to_wait(&pSMComm->waitQueue, &wait, TASK_INTERRUPTIBLE);

      /*
       * Consume the available answers, if any.
       */
      SCXLNXSMCommConsumeAnswers(pSMComm);

      /*
       * Check S-timeout.
       */
      SCXLNXSMCommReadTimeout(pSMComm, &sTimeout);

		if (SCXLNXSMCommTestSTimeout(pSMComm, sTimeout, &nRelativeTimeoutJiffies) == 0) {
         u32 nFirstCommand, nFirstFreeCommand;
         u32 nFirstAnswer, nFirstFreeAnswer;

         /* Lock concurrent access to the buffer while reading it */
         spin_lock(&(pSMComm->lock));
         nFirstCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstCommand);
         nFirstFreeCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeCommand);
         nFirstAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstAnswer);
         nFirstFreeAnswer = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeAnswer);
         spin_unlock(&(pSMComm->lock));

			if ((nFirstCommand == nFirstFreeCommand) && (nFirstAnswer == nFirstFreeAnswer)) {
				if (nRelativeTimeoutJiffies == MAX_SCHEDULE_TIMEOUT) {
					dprintk(KERN_DEBUG "SCXLNXSMCommPollingThread(%p) : prepare to sleep infinitely\n",
                     pSMComm);
				} else 		{
					dprintk(KERN_DEBUG "SCXLNXSMCommPollingThread(%p) : prepare to sleep 0x%lx jiffies\n",
                     pSMComm,
                     nRelativeTimeoutJiffies);
            }
            schedule_timeout(nRelativeTimeoutJiffies);
            dprintk(
						KERN_DEBUG "SCXLNXSMCommPollingThread(%p) : N_SM_EVENT signaled or timeout expired\n",
                  pSMComm);

            finish_wait(&pSMComm->waitQueue, &wait);
            goto begin;
         }
      }
      finish_wait(&pSMComm->waitQueue, &wait);
   }

#ifndef CONFIG_PREEMPT
		if (need_resched()) {
         schedule();
      }
#endif

end:
   SCXLNXSMCommSetCurrentTime(pSMComm);

   return 0;
}

/*----------------------------------------------------------------------------*/
/*
 * Sends the specified message through the specified SM communication.
 *
 * This function sends the message and returns immediately
 *
 * If pConn is not NULL, before sending the message, this function checks that it is still
 * valid by calling the function SCXLNXConnCheckMessageValidity
 *
 * Returns zero upon successful completion, or an appropriate error code upon
 * failure.
 */
static int SCXLNXSMCommSendMessage(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      SCX_COMMAND_MESSAGE* pMessage,
      SCXLNX_CONN_MONITOR* pConn,
      int bKillable)
{
   int nError;
   u32 nFirstFreeCommand;
   u32 nFirstCommand;

   dprintk(KERN_INFO "SCXLNXSMCommSendMessage(%p, %p)\n",
         pSMComm, pMessage);

   SCXLNXDumpMessage(pMessage);

   /*
    * Check if the current user space process
    * has received an interrupt.
    * If so, return immediately with correct error code
    */
	if (bKillable &&  (signal_pending (current))) {
      nError = -EINTR;
      goto error;
   }

	if (pConn != NULL && !SCXLNXConnCheckMessageValidity(pConn, pMessage)) {
      /* We must not send the message after all... */
      nError = -ENOTTY;
      goto error;
   }

   /*
    * Write the message in the message queue.
    */
retry:
   spin_lock(&pSMComm->lock);

   nFirstCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstCommand);
   nFirstFreeCommand = SCXLNXSMCommReadReg32(&pSMComm->pBuffer->nFirstFreeCommand);

	if (nFirstFreeCommand - nFirstCommand >= 32) {
      /* Command queue is full Yield to the secure world and try again */
      spin_unlock(&pSMComm->lock);
		dprintk(KERN_DEBUG "SCXLNXSMCommSendMessage(%p) : wake up the polling thread (Command queue full)\n",
            pSMComm);
      wake_up(&(pSMComm->waitQueue));
#ifndef CONFIG_PREEMPT
		if (need_resched()) {
         schedule();
      }
#endif

      /*
       * Check if the current user space process
       * has received an interrupt.
       * If so, return immediately with correct error code
       */
		if (bKillable &&  (signal_pending (current))) {
         nError = -EINTR;
         goto error;
      }

      goto retry;
   }

   (void)memcpy(
      &pSMComm->pBuffer->sCommandQueue[nFirstFreeCommand%SCX_SM_N_MESSAGE_QUEUE_CAPACITY],
      pMessage, sizeof(SCX_COMMAND_MESSAGE));

   nFirstFreeCommand++;

   SCXLNXSMCommWriteReg32(&pSMComm->pBuffer->nFirstFreeCommand, nFirstFreeCommand);

   spin_unlock(&pSMComm->lock);

   /*
    * Yield the PA
    */
	dprintk(KERN_DEBUG "SCXLNXSMCommSendMessage(%p) : wake up the polling thread\n",
         pSMComm);
   wake_up(&(pSMComm->waitQueue));

   /*
    * Successful completion.
    */

	dprintk(KERN_INFO "SCXLNXSMCommSendMessage(%p) : Success\n", pSMComm);
   return 0;

   /*
    * Error handling.
    */

error:
	dprintk(KERN_ERR "SCXLNXSMCommSendMessage(%p) : Failure (error %d)\n",
         pSMComm, nError);
   return nError;
}

#ifdef SMC_CACHE_OPTIM
/*----------------------------------------------------------------------------*/
/*Flush the shared memories cache					*/
/*----------------------------------------------------------------------------*/

static void sclnx_device_flush_shared_mem_cache(SCXLNX_CONN_MONITOR *pConn)
{
	u32 ret_loc;
	SCXLNX_SHMEM_DESC *pShmemDesc;
	SCXLNX_SHMEM_MONITOR *pShmemMonitor;
	pShmemMonitor = &(pConn->sSharedMemoryMonitor);
	ret_loc = down_interruptible(&(pShmemMonitor->sharedMemoriesMutex));
	list_for_each_entry(pShmemDesc, &(pShmemMonitor->sUsedSharedMemoryList), list)
	{
		v7_dma_flush_range(
				(u32)pShmemDesc->pBuffer,
				(u32)pShmemDesc->pBuffer + pShmemDesc->nAllocatedBufferSize);
	}
	up(&(pShmemMonitor->sharedMemoriesMutex));
}
#endif /*SMC_CACHE_OPTIM */
/* Sends the specified message through the specified SM communication.
 *
 * This function sends the message and waits for the corresponding answer
 * It may return if a signal needs to be delivered.
 *
 * If pConn is not NULL, before sending the message, this function checks that it is still
 * valid by calling the function SCXLNXConnCheckMessageValidity
 *
 * Returns zero upon successful completion, or an appropriate error code upon
 * failure.
 */
int SCXLNXSMCommSendReceive(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      SCX_COMMAND_MESSAGE *pMessage,
      SCX_ANSWER_MESSAGE *pAnswer,
      SCXLNX_CONN_MONITOR* pConn,
      int bKillable)
{
   int nError;
   SCXLNX_SM_ANSWER_STRUCT *pAnswerStructure = NULL;
   sigset_t nOldSet, nNewSet;

   dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive(%p) - message=%p answer=%p\n",
         pSMComm,
         pMessage,
         pAnswer);

	if (bKillable) {
		/*
      /*
       * only allow the SIGKILL signal to interrupt the operation
       */
      siginitsetinv(&nNewSet, sigmask(SIGKILL));
	} else {
		/*
      /*
       * do not allow any signal
       */
      siginitsetinv(&nNewSet, 0);
   }
   sigprocmask(SIG_BLOCK, &nNewSet, &nOldSet);

   pAnswerStructure = (SCXLNX_SM_ANSWER_STRUCT*) internal_kmalloc(sizeof(SCXLNX_SM_ANSWER_STRUCT), GFP_KERNEL);
	if (pAnswerStructure == NULL) {
      nError = -ENOMEM;
      goto error;
   }

   pAnswerStructure->pAnswer = pAnswer;
   pAnswerStructure->nSendingThreadAvailable = 1;
   pMessage->nOperationID = (u32) pAnswerStructure;

   init_completion(&pAnswerStructure->sAnswerEvent);

	if (pAnswer != NULL) {
      pAnswer->nSChannelStatus = S_PENDING;
   }

   /*
    * Send message if any.
    */


#ifdef SMC_CACHE_OPTIM
		switch (pMessage->nMessageType) {
		case SCX_MESSAGE_TYPE_OPEN_CLIENT_SESSION:
		case SCX_MESSAGE_TYPE_CLOSE_CLIENT_SESSION:
		case SCX_MESSAGE_TYPE_INVOKE_CLIENT_COMMAND:
		{
			sclnx_device_flush_shared_mem_cache(pConn);
		}
		break;
		default:
		break;
		}
#endif /*SMC_CACHE_OPTIM */
   /*
    * Send the command
    */

   nError = SCXLNXSMCommSendMessage(
         pSMComm,
         pMessage,
         pConn,
         bKillable);

	if (nError != 0) {
      dprintk(
				KERN_ERR "SCXLNXSMCommSendReceive(%p) : SCXLNXSMCommSendMessage failed (error %d) !\n",
            pSMComm, nError);
      goto error;
   }

   /* Now, wait for the answer */
	while (pAnswer != NULL) {
      nError = wait_for_completion_interruptible(&pAnswerStructure->sAnswerEvent);
		if (nError == -ERESTARTSYS) {
			/*
         /*
          * "wait for answer" operation failed, check why
          */
#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
		if (unlikely(freezing(current))) {
         powerPrintk(KERN_INFO "SCXLNXTZCommPollingThread: Entering refrigerator.\n");
         refrigerator();
         powerPrintk(KERN_INFO "SCXLNXTZCommPollingThread: Left refrigerator.\n");
      }
#endif

			if (bKillable && signal_pending(current)) {
				/*
            /*
             * The user space thread has received a signal
             * The only unblocked signal is SIGKILL
             * So the application should stop, return appropriate
             * error code.
             */

            nError = -EINTR;
				dprintk(KERN_INFO "SCXLNXSMCommSendReceive(%p) : Interrupted by user signal [%d]\n",
                     pSMComm, nError);

            /* Don't deallocate the answer structure yet, SCXLNXTZCommConsumeAnswers
               will do it (and will send a DESTROY_DEVICE_CONTEXT message as well) */
            pAnswerStructure->nSendingThreadAvailable = 0;

            /* restore signal maskss */
            sigprocmask(SIG_SETMASK, &nOldSet, NULL);

            return nError;
         }

         /*
          * ending up here means that wait_for_completion_interruptible()
          * has failed for an unknown reason. Loop again ?
          */
         continue;
      }

      /* the answer has been received, return */
      break;
   }


	if (pAnswer != NULL) {
		dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive(%p) : Message answer ready\n",
            pSMComm);

      /* print out a warning if the answer is not success */
		if (pAnswer->nSChannelStatus != S_SUCCESS) {
         dprintk(
					KERN_WARNING "SCXLNXSMCommSendReceive(%p) : Command failed with nSChannelStatus=0x%08x\n",
               pSMComm, pAnswer->nSChannelStatus);
         goto error;
      }
   }

   internal_kfree(pAnswerStructure);

   /* restore signal maskss */
   sigprocmask(SIG_SETMASK, &nOldSet, NULL);
   /* successful completion */
	dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive(%p) : Returns success\n",
         pSMComm);

   return 0;

error:
   /* restore signal maskss */
   sigprocmask(SIG_SETMASK, &nOldSet, NULL);

   internal_kfree(pAnswerStructure);

   return nError;
}

/*----------------------------------------------------------------------------*/

u32 SCXLNXSMCommRPCHandler(u32 nRPCId, u32 nRPCCommand, u32 nReserved1, u32 nReserved2)
{
   /*
    * Implementation note:
    * 1/ This routine is called in the context of the thread that has started
    *    the PA, i.e. SCXLNXSMCommPollingThread.
    * 2/ The L0 shared buffer is used to pass parameters from the PA SMC,
    *    and to return results to the PA SMC.
    */
   SCXLNX_SM_COMM_MONITOR*    pSMComm;

   u32 nRPCCall = RPC_SUCCESS;

	if (nRPCId != RPC_ID_SMODULE) {
      printk(KERN_ERR "SMC: RPC Handler: Invalid RPCId=0x%x, RPCCmd=0x%x [Ignored]\n",
               nRPCId, nRPCCommand);
      return RPC_ERROR_BAD_PARAMETERS;
   }


   pSMComm = g_pSMComm;

   BUG_ON(pSMComm == NULL);

	switch (nRPCCommand) {
   case RPC_CMD_YIELD:
      up(&(pSMComm->pubcrypto.HWALock));
      nRPCCall = SCXLNXSMCommPollingRoutine(pSMComm);
      down(&(pSMComm->pubcrypto.HWALock));
      g_L1SharedReady = true;
      break;

   case RPC_CMD_INIT:
      /*
       * Initialization phase in the normal world.
       * This is part of the PA initialization process in order to:
       *    > release the PA buffer (not required anymore)
       *    > rheck the protocol version (returned by the PA).
       */
      spin_lock(&(pSMComm->lock));
      nRPCCall = SCXLNXSMCommRPCInit(pSMComm);
      spin_unlock(&(pSMComm->lock));
      break;

   case RPC_CMD_TRACE:
      spin_lock(&(pSMComm->lock));
      secure_dprintk(KERN_INFO "%s\n", &(((SCHANNEL_L0_BUFFER_INPUT*)pSMComm->pL0SharedBuffer)->sReserved[RPC_TRACE_OFFSET]) );
      spin_unlock(&(pSMComm->lock));
      break;

   case RPC_CMD_PUBLIC_CRYPTO_INIT:
      nRPCCall = scxPublicCryptoRPCInit(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_ENABLE_CLOCK:
      nRPCCall = scxPublicCryptoRPCEnableClock(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_UPDATE:
      nRPCCall = scxPublicCryptoRPCUpdate(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_FINAL:
      nRPCCall = scxPublicCryptoRPCFinal(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_TERMINATE:
      nRPCCall = scxPublicCryptoRPCTerminate(&(pSMComm->pubcrypto), pSMComm->pL0SharedBuffer);
      break;

   case RPC_CMD_PUBLIC_CRYPTO_INVALIDATE_SESSION:
      pSMComm->pubcrypto.pCurrentSession = NULL;
      break;


   default:
      nRPCCall = RPC_ERROR_BAD_PARAMETERS;
		dprintk(KERN_ERR "SCXLNXSMCommRPCHandler(%p) : Invalid RPCCommand [0x%x]\n",
               pSMComm, nRPCCommand);
      break;
   }

   /*
    * OMAP3430 Secure ROM Code Functional Specification:
    *    L2 Cache is not used by SW which runs in Secure Mode.
    *    Thus, the non-Secure World�s software must ensure that any data
    *    in L2 Cache are coherent with memory before feeding such data to the
    *    Secure World for processing.
    */
   spin_lock(&(pSMComm->lock));
#ifdef SMC_CACHE_OPTIM
	v7_dma_flush_range(
		  (u32)pSMComm->pL0SharedBuffer,
		  (u32)pSMComm->pL0SharedBuffer + PAGE_SIZE);
	v7_dma_flush_range(
		  (u32)pSMComm->pBuffer,
		  (u32)pSMComm->pBuffer + PAGE_SIZE);
#else
   v7_flush_kern_cache_all();
#endif /*SMC_CACHE_OPTIM */
   spin_unlock(&(pSMComm->lock));

   return nRPCCall;
}

/*----------------------------------------------------------------------------*/

void SCXLNXSMCommReturnFromIRQ(void)
{
   SCXLNX_SM_COMM_MONITOR* pSMComm = g_pSMComm;

	if ((pSMComm != NULL) && (g_L1SharedReady)) {
      SCXLNXSMCommConsumeAnswers(pSMComm);
#ifndef CONFIG_PREEMPT
      schedule();
#endif
      SCXLNXSMCommSetCurrentTime(pSMComm);
      /*
       * OMAP3430 Secure ROM Code Functional Specification:
       *    L2 Cache is not used by SW which runs in Secure Mode.
       *    Thus, the non-Secure World�s software must ensure that any data
       *    in L2 Cache are coherent with memory before feeding such data to the
       *    Secure World for processing.
       */
      spin_lock(&(pSMComm->lock));
#ifdef SMC_CACHE_OPTIM
		v7_dma_flush_range(
			  (u32)pSMComm->pL0SharedBuffer,
			  (u32)pSMComm->pL0SharedBuffer + PAGE_SIZE);
		v7_dma_flush_range(
			  (u32)pSMComm->pBuffer,
			  (u32)pSMComm->pBuffer + PAGE_SIZE);
#else
      v7_flush_kern_cache_all();
#endif /*SMC_CACHE_OPTIM */
      spin_unlock(&(pSMComm->lock));
	} else {
      /* Nothing to do actually */
   }
}



/*----------------------------------------------------------------------------
 * Power management
 *----------------------------------------------------------------------------*/

/*
 * Perform a shutdown operation.
 * The routine does not return if the operation succeeds.
 * the routine returns an appropriate error code if
 * the operation fails.
 */
static inline int SCXLNXSMCommShutdown(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   int nError;
   SCX_COMMAND_MESSAGE sMessage;

   dprintk(KERN_INFO "SCXLNXSMCommShutdown(%p)\n", pSMComm);

   set_bit(SCXLNX_SM_COMM_FLAG_TERMINATING, &(pSMComm->nFlags));

   sMessage.nMessageType = SCX_MESSAGE_TYPE_POWER_MANAGEMENT;
   sMessage.sBody.sPowerManagementMessage.nPowerCommand = SCPM_PREPARE_SHUTDOWN;
   sMessage.sBody.sPowerManagementMessage.nSharedMemDescriptors[0] = 0;
   sMessage.sBody.sPowerManagementMessage.nSharedMemDescriptors[1] = 0;
   sMessage.sBody.sPowerManagementMessage.nSharedMemSize = 0;
   sMessage.sBody.sPowerManagementMessage.nSharedMemStartOffset = 0;

   nError = SCXLNXSMCommSendReceive(
         pSMComm,
         &sMessage,
         NULL, /* No answer required */
         NULL,
         0);

	if (nError != 0) {
      dprintk(
				KERN_ERR "SCXLNXSMCommShutdown(%p) : SCXLNXSMCommSendReceive failed (error %d) !\n",
            pSMComm, nError);
      return nError;
   }

	dprintk(KERN_INFO "SCXLNXSMCommShutdown(%p) : Waiting for polling thread death...\n",
         pSMComm);
   wake_up(&(pSMComm->waitQueue));
   wait_for_completion(&(pSMComm->pollingThreadDeath));

	dprintk(KERN_INFO "SCXLNXSMCommShutdown(%p) : Success\n", pSMComm);

   return 0;
}


/*
 * Handles all the power management calls.
 * The nOperation is the type of power management
 * operation to be performed.
 */
int SCXLNXSMCommPowerManagement(
      SCXLNX_SM_COMM_MONITOR *pSMComm,
      SCXLNX_SM_POWER_OPERATION nOperation)
{
   u32 nStatus;
   int nError = 0;

	dprintk(KERN_INFO "SCXLNXSMCommPowerManagement(%p, %d)\n", pSMComm, nOperation);

	if ((test_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags))) == 0) {
		dprintk(KERN_INFO "SCXLNXSMCommPowerManagement(%p) : succeeded (not started)\n", pSMComm);
      return 0;
   }

   nStatus = ((SCXLNXSMCommReadReg32(&(pSMComm->pBuffer->nStatus_S))
         & SCX_SM_STATUS_POWER_STATE_MASK)
         >> SCX_SM_STATUS_POWER_STATE_SHIFT);

	switch (nOperation) {
   case SCXLNX_SM_POWER_OPERATION_SHUTDOWN:

		switch (nStatus) {
      case SCX_SM_POWER_MODE_ACTIVE:
         nError = SCXLNXSMCommShutdown(pSMComm);

			if (nError) {
				dprintk(KERN_ERR "SCXLNXSMCommPowerManagement(%p) : Failed with error code 0x%08x\n", pSMComm, nError);
            goto error;
         }
         break;

      default:
			dprintk(KERN_ERR "SCXLNXSMCommPowerManagement(%p) : Power command not allowed in current state %d\n", pSMComm, nStatus);
         nError = -ENOTTY;
         goto error;
      }
      break;


   default:
      nError = -ENOTSUPP;
		dprintk(KERN_ERR "SCXLNXSMCommPowerManagement(%p) : Operation not supported [%d]\n", pSMComm, nOperation);
      goto error;
   }

	dprintk(KERN_INFO "SCXLNXSMCommPowerManagement(%p) : succeeded\n", pSMComm);
   return 0;

error:
   return nError;
}


/*
 * Saves the context of the secure world
 */
#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
int SCXLNXCommSaveContext(uint32_t nPhysicalAddress)
{
   int nError;

   nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_CONTEXT_SAVE_RESTORE,
                                          0,
                                          FLAG_START_HAL_CRITICAL,
                                          4,
                                          nPhysicalAddress, /* Physical address in sdram where to save the secure ram (u8*) */
                                          0x1E, /* First Dma channel */
                                          0x1F, /* Second Dma channel */
                                          1     /* The direction. 1 for save, 0 for restore. */
                                        );

	if (nError != API_HAL_RET_VALUE_OK) {
      powerPrintk("Context save Error=%d PhysAddr=0x%x", nError, nPhysicalAddress);
      return -EFAULT;
   }

   powerPrintk("Context save OK PhysAddr=0x%x", nPhysicalAddress);
   return 0;
}
#endif


/*----------------------------------------------------------------------------
 * Communication initialization and termination
 *----------------------------------------------------------------------------*/

/*
 * Resets the communication descriptor
 */
void SCXLNXSMCommReset(
            SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   spin_lock_init(&(pSMComm->lock));
   pSMComm->nFlags = 0;
   pSMComm->pBuffer = NULL;
   pSMComm->pL0SharedBuffer = NULL;
   pSMComm->pPAInfo = NULL;
   pSMComm->pPABuffer = NULL;
   init_waitqueue_head(&(pSMComm->waitQueue));
   init_completion(&(pSMComm->pollingThreadDeath));

   pSMComm->bSDPInitialized = false;
   INIT_LIST_HEAD(&(pSMComm->pubcrypto.sessions));
   pSMComm->pubcrypto.nAesSession = 0;
   pSMComm->pubcrypto.nDesSession = 0;
   pSMComm->pubcrypto.nShaSession = 0;
}


int SCXLNXSMCommInit(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   u32 nError;

   dprintk(KERN_INFO "SCXLNXSMCommInit(%p)\n", pSMComm);

   SCXLNXSMCommReset(pSMComm);

#ifdef SMODULE_SMC_OMAP3430_POWER_MANAGEMENT
   init_timer(&pSMComm->pubcrypto.pPowerManagementTimer);
#endif /* SMODULE_SMC_OMAP3430_POWER_MANAGEMENT */

   nError = scxPublicCryptoInit(&(pSMComm->pubcrypto));
	if (nError != PUBLIC_CRYPTO_OPERATION_SUCCESS) {
      goto error;
   }


   g_pSMComm = pSMComm;


   return 0;

error:
   scxPublicCryptoTerminate(&(pSMComm->pubcrypto));
   return -EFAULT;
}


/*----------------------------------------------------------------------------*/
/*
 * Initializes the SE (SDP, SRAM size, RPC handler).
 *
 * Returns 0 upon success or appropriate error code
 * upon failure
 */
static int SCXLNXSMSDPInit(
      SCXLNX_SM_COMM_MONITOR* pSMComm,
      u32 nSDPBackingStoreAddr, u32 nSDPBkExtStoreAddr)
{
   int nError;
	unsigned int nCrc;

   dprintk(KERN_INFO "SCXLNXSMSDPInit\n");

	if (!pSMComm->bSDPInitialized) {
      /* Secure CRC Read  -------------------------------------------------------*/
      dprintk(KERN_INFO "SCXSMSDPInit: Secure CRC Read...\n");
		nCrc = SEC_ENTRY_pub2sec_dispatcher(API_HAL_KM_CRC_READ, 0, 0, 0, 0, 0, 0, 0);
		printk(KERN_INFO "SMC: SecureCRC=0x%08X\n", nCrc);

      /* SRAM RESIZE  -----------------------------------------------------------*/
      /* 60KB because the last 4KB are already used */
      dprintk(KERN_INFO "SCXLNXSMSDPInit: SRAM resize (60KB)...\n");
      nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_SEC_RAM_RESIZE,
                                             0,
                                             FLAG_FIQ_ENABLE|FLAG_START_HAL_CRITICAL,
                                             1,
															SEC_RAM_SIZE_60KB,
															0, 0, 0);
		if (nError == API_HAL_RET_VALUE_OK) {
         dprintk(KERN_INFO "SCXLNXSMSDPInit: SRAM resize OK\n");
		} else {
         dprintk(KERN_ERR "SCXLNXSMSDPInit: SRAM resize failed [0x%x]\n", nError);
         goto error;
      }

      /* SDP INIT ---------------------------------------------------------------*/
      dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP runtime init... (nSDPBackingStoreAddr=%x, nSDPBkExtStoreAddr=%x)\n",
                             nSDPBackingStoreAddr, nSDPBkExtStoreAddr);
      nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_SDP_RUNTIME_INIT,
                                             0,
                                             FLAG_FIQ_ENABLE|FLAG_START_HAL_CRITICAL,
                                             2,
                                             nSDPBackingStoreAddr,
															nSDPBkExtStoreAddr,
															0, 0);
		if (nError == API_HAL_RET_VALUE_SDP_RUNTIME_INIT_ERROR) {
			dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP runtime init already launched [0x%x]\n", nError);
		} else if (nError == API_HAL_RET_VALUE_OK) {
         dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP runtime init OK\n");
		} else {
         dprintk(KERN_ERR "SCXLNXSMSDPInit: SDP runtime init failed [0x%x]\n", nError);
         goto error;
      }

      /* RPC INIT  --------------------------------------------------------------*/
      dprintk(KERN_INFO "SCXLNXSMSDPInit: RPC init...\n");
      nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_SEC_RPC_INIT,
                                             0,
                                             FLAG_START_HAL_CRITICAL,
                                             1,
															(u32)(u32 (*const)(u32, u32, u32, u32)) &rpc_handler,
															0, 0, 0);
		if (nError == API_HAL_RET_VALUE_OK) {
         dprintk(KERN_INFO "SCXLNXSMSDPInit: RPC init OK\n");
		} else {
         dprintk(KERN_ERR "SCXLNXSMSDPInit: RPC init failed [0x%x]\n", nError);
         goto error;
      }

      pSMComm->bSDPInitialized = true;
	} else {
      dprintk(KERN_INFO "SCXLNXSMSDPInit: SDP already initilized... nothing to do\n");
   }

   return 0;

error:
   return -EFAULT;
}


/*----------------------------------------------------------------------------*/
/*
 * Starts the SM.
 *
 * Returns 0 upon success or appropriate error code
 * upon failure
 */
int SCXLNXSMCommStart(
      SCXLNX_SM_COMM_MONITOR* pSMComm,
      u32 nSDPBackingStoreAddr, u32 nSDPBkExtStoreAddr,
      u8* pPABufferVAddr, u32 nPABufferSize,
      u8* pPropertiesBuffer, u32 nPropertiesBufferLength)
{
   SCHANNEL_C1S_BUFFER* pL1SharedBuffer = NULL;
   SCHANNEL_L0_BUFFER_INPUT* pL0SharedBuffer = NULL;
   SCHANNEL_L0_BUFFER_SMC_INIT_INPUT* pSMCInitInput;
   NS_PA_INFO*   paInfo = NULL;
   int  nError;

   /*
    * Implementation notes:
    *
    * 1/ The PA buffer (pPABufferVAddr) is now owned by this function.
    *    In case of error, it is responsible for releasing the buffer.
    *
    * 2/ The PA Info and PA Buffer will be freed through a RPC call
    *    at the beginning of the PA entry in the SE.
    */

   dprintk(KERN_INFO "SCXLNXSMCommStart(%p)\n", pSMComm);

	if ((test_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags))) != 0) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : The SM is already started\n", pSMComm);
      nError = -EFAULT;
      goto error1;
   }

	if ((sizeof(SCHANNEL_L0_BUFFER_INPUT) != PAGE_SIZE) || (sizeof(SCHANNEL_C1S_BUFFER) != PAGE_SIZE)) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : The L0 or L1 structure size is incorrect !\n", pSMComm);
      nError = -EFAULT;
      goto error1;
   }

   nError = SCXLNXSMSDPInit(pSMComm, nSDPBackingStoreAddr, nSDPBkExtStoreAddr);
	if (nError != 0) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : SDP init failed with %x!\n", pSMComm, nError);
      goto error1;
   }

   paInfo = (NS_PA_INFO*)internal_kmalloc(sizeof(NS_PA_INFO), GFP_KERNEL);

   pL0SharedBuffer = (SCHANNEL_L0_BUFFER_INPUT*)internal_get_zeroed_page(GFP_KERNEL);
   pL1SharedBuffer = (SCHANNEL_C1S_BUFFER*)internal_get_zeroed_page(GFP_KERNEL);

	if ((paInfo == NULL) || (pL0SharedBuffer == NULL) || (pL1SharedBuffer == NULL)) {
		dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : Out of memory\n", pSMComm);
      nError = -ENOMEM;
      goto error1;
   }

   /*
    * Ensure the page storing the SM communication buffer is mapped.
    */
#ifdef KERNEL_2_6_27
   set_page_locked(virt_to_page(pL0SharedBuffer));
   set_page_locked(virt_to_page(pL1SharedBuffer));
#else
   SetPageLocked(virt_to_page(pL0SharedBuffer));
   SetPageLocked(virt_to_page(pL1SharedBuffer));
#endif

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : L0SharedBuffer={0x%x, 0x%x}\n",
            pSMComm, (u32)pL0SharedBuffer, (u32)__pa(pL0SharedBuffer));

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : L1SharedBuffer={0x%x, 0x%x}\n",
            pSMComm, (u32)pL1SharedBuffer, (u32)__pa(pL1SharedBuffer));

   pSMComm->pPAInfo = paInfo;
   pSMComm->pPABuffer = pPABufferVAddr;
   pSMComm->pL0SharedBuffer = pL0SharedBuffer;
   pSMComm->pBuffer = pL1SharedBuffer;

   paInfo->pCertificate = (void*)__pa(pPABufferVAddr);
   paInfo->pParameters = (void*)__pa(pL0SharedBuffer);
   paInfo->pResults = (void*)__pa(pL0SharedBuffer);

   memset(pL0SharedBuffer, 0, sizeof(SCHANNEL_L0_BUFFER_INPUT));
   memset(pL1SharedBuffer, 0, sizeof(SCHANNEL_C1S_BUFFER));

   pSMCInitInput = (SCHANNEL_L0_BUFFER_SMC_INIT_INPUT*)pL0SharedBuffer;

   pSMCInitInput->nL1Command = SCX_SM_SMC_INIT;
   pSMCInitInput->nL1SharedBufferLength = sizeof(SCHANNEL_C1S_BUFFER);
   pSMCInitInput->nL1SharedBufferPhysAddr = __pa(pL1SharedBuffer);

	pSMCInitInput->nBackingStoreAddr   = nSDPBackingStoreAddr;
	pSMCInitInput->nBackExtStorageAddr = nSDPBkExtStoreAddr;
   pSMCInitInput->nPropertiesBufferLength = nPropertiesBufferLength;
	if (nPropertiesBufferLength == 0) {
      pSMCInitInput->pPropertiesBuffer[0] = 0;
	} else {
      memcpy(pSMCInitInput->pPropertiesBuffer, pPropertiesBuffer, nPropertiesBufferLength);
   }

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : System Configuration (%d bytes)\n",
            pSMComm, pSMCInitInput->nPropertiesBufferLength);

	dprintk(KERN_INFO "SCXLNXSMCommStart(%p) : Starting PA (%d bytes)...\n",
            pSMComm, nPABufferSize);

   /*
    * Create the PA running thread.
    */

   nError = kernel_thread(SCXLNXSMCommPollingThread, pSMComm, CLONE_KERNEL);
	if (nError < 0) {
      dprintk(
				KERN_ERR "SCXLNXSMCommStart(%p) : Kernel_thread failed [%d] !\n",
            pSMComm, nError);
      goto error2;
   }

   set_bit(SCXLNX_SM_COMM_FLAG_POLLING_THREAD_STARTED, &(pSMComm->nFlags));

   return 0;

error1:
   internal_kfree(pPABufferVAddr);
   internal_kfree(paInfo);
	if (pL0SharedBuffer != NULL) {
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pL0SharedBuffer));
#else
   ClearPageLocked(virt_to_page(pL0SharedBuffer));
#endif
      internal_free_page((unsigned long)pL0SharedBuffer);
   }
	if (pL1SharedBuffer != NULL) {
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pL1SharedBuffer));
#else
   ClearPageLocked(virt_to_page(pL1SharedBuffer));
#endif
      internal_free_page((unsigned long)pL1SharedBuffer);
   }

error2:
   /*
    * Error handling.
    */

	dprintk(KERN_ERR "SCXLNXSMCommStart(%p) : Failure [%d]\n",
            pSMComm, nError);

   SCXLNXSMCommStop(pSMComm);

   return nError;
}

/*----------------------------------------------------------------------------*/

void SCXLNXSMCommStop(SCXLNX_SM_COMM_MONITOR* pSMComm)
{
   int  nError;

   spin_lock(&(pSMComm->lock));

   dprintk(KERN_INFO "SCXLNXSMCommStop(%p)\n", pSMComm);

   /* PA unload */
   nError = SEC_ENTRY_pub2sec_dispatcher( API_HAL_PA_UNLOAD_ALL,
                                          0,
                                          FLAG_START_HAL_CRITICAL,
														0, 0, 0, 0, 0);
	if (nError != API_HAL_RET_VALUE_OK) {
		dprintk(KERN_ERR "SCXLNXSMCommStop(%p) : SM Unload failed [0x%x]\n",
               pSMComm, nError);
   }

	if (pSMComm->pBuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : L1SharedBuffer released\n", pSMComm);
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pSMComm->pBuffer));
#else
   ClearPageLocked(virt_to_page(pSMComm->pBuffer));
#endif
      internal_free_page((unsigned long) pSMComm->pBuffer);
      pSMComm->pBuffer = NULL;
   }

	if (pSMComm->pL0SharedBuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : L0SharedBuffer released\n", pSMComm);
#ifdef KERNEL_2_6_27
   clear_page_locked(virt_to_page(pSMComm->pL0SharedBuffer));
#else
   ClearPageLocked(virt_to_page(pSMComm->pL0SharedBuffer));
#endif
      internal_free_page((unsigned long) pSMComm->pL0SharedBuffer);
      pSMComm->pL0SharedBuffer = NULL;
   }

	if (pSMComm->pPABuffer != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : PA Buffer released\n", pSMComm);
      internal_kfree(pSMComm->pPABuffer);
      pSMComm->pPABuffer = NULL;
   }

	if (pSMComm->pPAInfo != NULL) {
		dprintk(KERN_INFO "SCXLNXSMCommStop(%p) : PA Info released\n", pSMComm);
      internal_kfree(pSMComm->pPAInfo);
      pSMComm->pPAInfo = NULL;
   }

   spin_unlock(&(pSMComm->lock));
}

/*----------------------------------------------------------------------------*/

/*
 * Attempt to terminate the communication.
 */
void SCXLNXSMCommTerminate(
      SCXLNX_SM_COMM_MONITOR *pSMComm)
{
   dprintk(KERN_INFO "SCXLNXSMCommTerminate(%p)\n", pSMComm);

   spin_lock(&(pSMComm->lock));

   scxPublicCryptoTerminate(&(pSMComm->pubcrypto));

   g_pSMComm = NULL;

   spin_unlock(&(pSMComm->lock));
}

