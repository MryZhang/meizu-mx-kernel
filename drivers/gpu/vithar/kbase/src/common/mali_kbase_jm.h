/*
 *
 * (C) COPYRIGHT 2011 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 * 
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 */



/**
 * @file mali_kbase_jm.h
 * Job Manager Low-level APIs.
 */

#ifndef _KBASE_JM_H_
#define _KBASE_JM_H_

/**
 * @addtogroup base_api
 * @{
 */

/**
 * @addtogroup base_kbase_api
 * @{
 */


/**
 * @addtogroup kbase_jm Job Manager Low-level APIs
 * @{
 *
 */

static INLINE int kbasep_jm_is_js_free(kbase_device *kbdev, int js, kbase_context *kctx)
{
	OSK_ASSERT( kbdev != NULL );
	OSK_ASSERT( 0 <= js && js < kbdev->nr_job_slots  );

#if BASE_HW_ISSUE_5713
	/* On r0p0 we need to ensure that there is no job in the current registers as well
	 * because it is unsafe to soft-stop a slot when a second job is queued */
	return !kbase_reg_read(kbdev, JOB_SLOT_REG(js, JSn_COMMAND_NEXT), kctx) &&
	       !kbase_reg_read(kbdev, JOB_SLOT_REG(js, JSn_COMMAND), kctx);
#else
	return !kbase_reg_read(kbdev, JOB_SLOT_REG(js, JSn_COMMAND_NEXT), kctx);
#endif /* BASE_HW_ISSUE_5713 */
}

/**
 * This checks that:
 * - there is enough space in the GPU's buffers (JSn_NEXT and JSn_HEAD registers) to accomodate the job.
 * - there is enough space to track the job in a our Submit Slots. Note that we have to maintain space to
 *   requeue one job in case the next registers on the hardware need to be cleared.
 * - the slot is not blocked (due to PRLAM-5713 workaround)
 */
static INLINE mali_bool kbasep_jm_is_submit_slots_free(kbase_device *kbdev, int js, kbase_context *kctx)
{
	OSK_ASSERT( kbdev != NULL );
	OSK_ASSERT( 0 <= js && js < kbdev->nr_job_slots  );
	
	if (osk_atomic_get(&kbdev->reset_gpu) != KBASE_RESET_GPU_NOT_PENDING)
	{
		/* The GPU is being reset - so prevent submission */
		return MALI_FALSE;
	}

#if BASE_HW_ISSUE_5713
	return (mali_bool)( !kbdev->jm_slots[js].submission_blocked_for_soft_stop
	                    && kbasep_jm_is_js_free(kbdev, js, kctx)
	                    && kbdev->jm_slots[js].submitted_nr < (BASE_JM_SUBMIT_SLOTS-2) );
#else
	return (mali_bool)( kbasep_jm_is_js_free(kbdev, js, kctx)
	                    && kbdev->jm_slots[js].submitted_nr < (BASE_JM_SUBMIT_SLOTS-2) );
#endif
}

/**
 * Initialize a submit slot
 */
static INLINE void kbasep_jm_init_submit_slot(  kbase_jm_slot *slot )
{
	slot->submitted_nr = 0;
	slot->submitted_head = 0;
#if BASE_HW_ISSUE_5713
	slot->submission_blocked_for_soft_stop = MALI_FALSE;
#endif
}

/**
 * Find the atom at the idx'th element in the queue without removing it, starting at the head with idx==0.
 */
static INLINE kbase_jd_atom* kbasep_jm_peek_idx_submit_slot( kbase_jm_slot *slot, u8 idx )
{
	u8 pos;
	kbase_jd_atom *katom;

	OSK_ASSERT( idx < BASE_JM_SUBMIT_SLOTS );

	pos = (slot->submitted_head + idx) & BASE_JM_SUBMIT_SLOTS_MASK;
	katom = slot->submitted[pos];

	return katom;
}

/**
 * Pop front of the submitted
 */
static INLINE kbase_jd_atom* kbasep_jm_dequeue_submit_slot( kbase_jm_slot *slot )
{
	u8 pos;
	kbase_jd_atom *katom;

	pos = slot->submitted_head & BASE_JM_SUBMIT_SLOTS_MASK;
	katom = slot->submitted[pos];
	slot->submitted[pos] = NULL; /* Just to catch bugs... */
	OSK_ASSERT(katom);

	/* rotate the buffers */
	slot->submitted_head = (slot->submitted_head + 1) & BASE_JM_SUBMIT_SLOTS_MASK;
	slot->submitted_nr--;

	OSK_PRINT_INFO( OSK_BASE_JM, "katom %p new head %u",
	                (void *)katom, (unsigned int)slot->submitted_head);

	return katom;
}

/* Pop back of the submitted queue (unsubmit a job)
 */
static INLINE kbase_jd_atom *kbasep_jm_dequeue_tail_submit_slot( kbase_jm_slot *slot )
{
	u8 pos;

	slot->submitted_nr--;

	pos = (slot->submitted_head + slot->submitted_nr) & BASE_JM_SUBMIT_SLOTS_MASK;

	return slot->submitted[pos];
}

static INLINE u8 kbasep_jm_nr_jobs_submitted( kbase_jm_slot *slot )
{
	return slot->submitted_nr;
}


/**
 * Push back of the submitted
 */
static INLINE void kbasep_jm_enqueue_submit_slot( kbase_jm_slot *slot, kbase_jd_atom *katom )
{
	u8 nr;
	u8 pos;
	nr = slot->submitted_nr++;
	OSK_ASSERT(nr < BASE_JM_SUBMIT_SLOTS);
	
	pos = (slot->submitted_head + nr) & BASE_JM_SUBMIT_SLOTS_MASK;
	slot->submitted[pos] = katom;
}

/**
 * @brief Submit a job to a certain job-slot
 *
 * The caller must check kbasep_jm_is_submit_slots_free() != MALI_FALSE before calling this.
 *
 * The following locking conditions are made on the caller:
 * - it must hold the kbasep_js_device_data::runpoool_irq::lock
 *  - This is to access the kbase_context::as_nr
 *  - In any case, the kbase_js code that calls this function will always have
 * this lock held.
 * - it must hold kbdev->jm_slots[ \a s ].lock
 */
void kbase_job_submit_nolock(kbase_device *kbdev, kbase_jd_atom *katom, int js);

/**
 * @brief Complete the head job on a particular job-slot
 */
void kbase_job_done_slot(kbase_device *kbdev, int s, u32 completion_code, u64 job_tail, kbasep_js_tick *end_timestamp);

/**
 * @brief Obtain the lock for a job slot.
 *
 * This function also returns the structure for the specified job slot to simplify the code
 *
 * @param[in] kbdev     Kbase device pointer
 * @param[in] js        The job slot number to lock
 *
 * @return  The job slot structure
 */
static INLINE kbase_jm_slot *kbase_job_slot_lock(kbase_device *kbdev, int js)
{
#if BASE_HW_ISSUE_7347
	osk_spinlock_irq_lock(&kbdev->jm_slot_lock);
#else
	osk_spinlock_irq_lock(&kbdev->jm_slots[js].lock);
#endif
	return &kbdev->jm_slots[js];
}

/**
 * @brief Release the lock for a job slot
 *
 * @param[in] kbdev     Kbase device pointer
 * @param[in] js        The job slot number to unlock
 */
static INLINE void kbase_job_slot_unlock(kbase_device *kbdev, int js)
{
#if BASE_HW_ISSUE_7347
	osk_spinlock_irq_unlock(&kbdev->jm_slot_lock);
#else
	osk_spinlock_irq_unlock(&kbdev->jm_slots[js].lock);
#endif
}

/** @} */ /* end group kbase_jm */
/** @} */ /* end group base_kbase_api */
/** @} */ /* end group base_api */

#endif /* _KBASE_JM_H_ */
