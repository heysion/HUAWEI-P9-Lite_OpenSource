


#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

#if (_PRE_OS_VERSION_RAW == _PRE_OS_VERSION)
/*****************************************************************************
  1 头文件包含
*****************************************************************************/
#include "oal_hcc_slave_if.h"
#include "hal_hi1102_reset.h"
#include "wlan_spec.h"
#include "wlan_mib.h"
#include "hal_device.h"
#include "hal_hi1102_soc.h"
#include "hal_hi1102_mac_reg.h"
#include "hal_hi1102_soc_reg.h"
#include "hal_hi1102_phy_reg.h"

#include "oal_mem.h"
#include "mac_resource.h"
#include "stack_cr4.h"
#include "DMA.h"
#include "dmac_pm_sta.h"
#include "dmac_rx_data.h"
#include "frw_timer.h"
#ifdef _PRE_WLAN_FEATURE_BTCOEX
#include "dmac_btcoex.h"
#endif
#include "pm_extern.h"

#undef  THIS_FILE_ID
#define THIS_FILE_ID OAM_FILE_ID_DMAC_PM_STA_C



/*****************************************************************************
  2 全局变量定义
*****************************************************************************/
extern oal_uint8  dmac_psm_is_hw_queues_empty(mac_device_stru  *pst_device);
extern oal_uint8  dmac_psm_is_tid_queues_empty(dmac_vap_stru  *pst_dmac_vap);

oal_uint32  g_ul_wait_hw = 0;
oal_uint32  g_ul_wait_tid = 0;
oal_uint32  g_ul_wait_rx_irq = 0;
oal_uint32  g_ul_coex_cnt  = 0;
oal_uint32  g_ul_wait_scanning = 0;
oal_uint32  g_ul_wait_dbac = 0;

/*****************************************************************************
  3 函数实现
*****************************************************************************/

oal_void dmac_psm_sync_tsf_to_sta(oal_void)
{
    hal_to_dmac_vap_stru                 st_sta_hal_vap;

    /* 关闭pa */
    dmac_psm_cbb_stopwork();

    /* 将ap0的tsf和tbtt同步到sta0 */
    st_sta_hal_vap.uc_vap_id = 4;

    hal_sta_tsf_restore(&st_sta_hal_vap);

    /* 将ap1的tsf和tbtt同步到sta1 */
    st_sta_hal_vap.uc_vap_id = 5;

    hal_sta_tsf_restore(&st_sta_hal_vap);

    return;
}

oal_void dmac_psm_sync_tsf_to_ap(oal_void)
{
    hal_to_dmac_vap_stru    st_sta_hal_vap;

    /* 将sta0的tsf和tbtt同步到ap0 */
    st_sta_hal_vap.uc_vap_id = 4;

    //低功耗唤醒后sta->ap的同步过程中不能进行ap——>sta的同步
    hal_sta_tsf_save(&st_sta_hal_vap, OAL_FALSE);

    /* 将sta1的tsf和tbtt同步到ap1 */
    st_sta_hal_vap.uc_vap_id = 5;

    hal_sta_tsf_save(&st_sta_hal_vap, OAL_FALSE);

    return;

}

oal_uint8 dmac_psm_check_hw_txq_state(oal_void)
{
    mac_device_stru             *pst_device;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device      = mac_res_get_dev(0);


    /*检查接收硬件发送队列和TID队列是否空*/
    if(OAL_FALSE == dmac_psm_is_hw_queues_empty(pst_device))
    {
        return OAL_FALSE;
    }

    return OAL_TRUE;
}


oal_uint8 dmac_psm_check_txrx_state(oal_void)
{
    hal_to_dmac_device_stru     *pst_hal_device;
    mac_device_stru             *pst_device;
    dmac_vap_stru                  *pst_dmac_vap;
    oal_uint8                       uc_vap_idx;

    pst_device      = mac_res_get_dev(0);
    if (OAL_UNLIKELY(OAL_PTR_NULL == pst_device ))
    {
        OAM_ERROR_LOG0(0, OAM_SF_ANY, "{dmac_psm_check_txrx_state::pst_device[id:0] is NULL!}");
        return OAL_FALSE;
    }
    pst_hal_device  = pst_device->pst_device_stru;



#ifdef _PRE_WLAN_FEATURE_BTCOEX
    if(pst_hal_device->st_btcoex_btble_status.un_bt_status.st_bt_status.bit_bt_on)
    {
        g_ul_coex_cnt++;
    }
#endif

    /* 正在扫描不睡眠 */
    if (MAC_SCAN_STATE_RUNNING == pst_device->en_curr_scan_state)
    {
        /* 仅在host允许睡的情况下计数++,此计数是为了验证低功耗在扫描过程中睡眠了 */
        if (OAL_TRUE == PM_WLAN_IsHostSleep())
        {
            g_ul_wait_scanning++;
        }
        return OAL_FALSE;
    }

    /* DBAC running 不睡眠 */
    if(OAL_TRUE == mac_is_dbac_running(pst_device))
    {
        g_ul_wait_dbac++;
        return OAL_FALSE;
    }

    /*检查接收硬件发送队列和TID队列是否空*/
    if(OAL_FALSE == dmac_psm_is_hw_queues_empty(pst_device))
    {
        //OAL_IO_PRINT("pm check:tx hw queues not empty\r\n");
        g_ul_wait_hw++;
        return OAL_FALSE;
    }

    for (uc_vap_idx = 0; uc_vap_idx < pst_device->uc_vap_num; uc_vap_idx++)
    {
        pst_dmac_vap = mac_res_get_dmac_vap(pst_device->auc_vap_id[uc_vap_idx]);

        if (WLAN_VAP_MODE_BSS_STA == pst_dmac_vap->st_vap_base_info.en_vap_mode)
        {

            if(OAL_FALSE == dmac_psm_is_tid_queues_empty(pst_dmac_vap))
            {
                //OAL_IO_PRINT("pm check:tx tid queues not empty\r\n");
                g_ul_wait_tid++;
                 return OAL_FALSE;
            }
        }
    }

    /*检查接收中断是否有残留*/
    if (!oal_dlist_is_empty(&(pst_hal_device->ast_rx_isr_info_list[0])))
    {
        //OAL_IO_PRINT("ast_rx_isr_info_list[0] IS NOT EMPTY\r\n");
        g_ul_wait_rx_irq++;
        return OAL_FALSE;

    }

    if (!oal_dlist_is_empty(&(pst_hal_device->ast_rx_isr_info_list[1])))
    {
        //OAL_IO_PRINT("ast_rx_isr_info_list[1] IS NOT EMPTY\r\n");
        g_ul_wait_rx_irq++;
         return OAL_FALSE;
    }

    return OAL_TRUE;

}

oal_uint8  dmac_psm_is_fake_queues_empty(oal_void)
{
    mac_device_stru                     *pst_device;
    hal_to_dmac_device_stru             *pst_hal_device;
    oal_uint8                           uc_fake_q;
    oal_uint8                           uc_ret = OAL_TRUE;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device      = mac_res_get_dev(0);
    pst_hal_device  = pst_device->pst_device_stru;

    /* 目前检查完所有的fake_q,稳定后有false立刻break,不睡眠 */
    for (uc_fake_q = 0; uc_fake_q <= HAL_TX_FAKE_QUEUE_BGSCAN_ID; uc_fake_q++)
    {
        if (dmac_tx_fake_queue_empty_assert(pst_hal_device, uc_fake_q, THIS_FILE_ID) != OAL_TRUE)
        {
            uc_ret = OAL_FALSE;
        }
    }

    return uc_ret;
}


oal_void dmac_psm_save_tcm (hal_to_dmac_device_stru  *pst_hal_device)
{
    hal_reset_reg_save(pst_hal_device, HAL_RESET_HW_TYPE_TCM);

}

oal_void dmac_psm_save_start_dma (oal_uint8* puc_ch0,oal_uint8* puc_ch1,oal_uint8* puc_ch2)
{
    hal_to_dmac_device_stru        *pst_hal_device;
    mac_device_stru                *pst_device;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device      = mac_res_get_dev(0);
    pst_hal_device  = pst_device->pst_device_stru;

    hal_reset_reg_dma_save(pst_hal_device,puc_ch0,puc_ch1,puc_ch2);
}

#ifdef _PRE_WLAN_FEATURE_EDCA_OPT

oal_void dmac_psm_save_dutycycle (mac_device_stru  *pst_device)
{
    oal_uint32 aul_reg_value[3];

    /* 读取mac统计寄存器 */
    hal_get_txrx_frame_time(pst_device->pst_device_stru, aul_reg_value);
   /*保存进入低功耗前的tx rx时间*/
    pst_device->ul_rx_dir_duty_lp += aul_reg_value[0];
    pst_device->ul_rx_nondir_duty_lp += aul_reg_value[1];
    pst_device->ul_duty_ratio_lp += aul_reg_value[2];
}
#endif

oal_void dmac_psm_save_mac_statistics_data(mac_device_stru  *pst_device)
{
    hal_mac_key_statis_info_stru   st_mac_key_statis_info;

    hal_get_mac_statistics_data(pst_device->pst_device_stru, &st_mac_key_statis_info);

    pst_device->st_mac_key_statis_info.ul_tkipccmp_rep_fail_cnt += st_mac_key_statis_info.ul_tkipccmp_rep_fail_cnt;
    pst_device->st_mac_key_statis_info.ul_tx_mpdu_cnt  += st_mac_key_statis_info.ul_tx_mpdu_cnt;
    pst_device->st_mac_key_statis_info.ul_rx_passed_mpdu_cnt  += st_mac_key_statis_info.ul_rx_passed_mpdu_cnt;
    pst_device->st_mac_key_statis_info.ul_rx_failed_mpdu_cnt  += st_mac_key_statis_info.ul_rx_failed_mpdu_cnt;
    pst_device->st_mac_key_statis_info.ul_rx_tkipccmp_mic_fail_cnt  += st_mac_key_statis_info.ul_rx_tkipccmp_mic_fail_cnt;
    pst_device->st_mac_key_statis_info.ul_key_search_fail_cnt += st_mac_key_statis_info.ul_key_search_fail_cnt;
}



oal_uint8 dmac_psm_clean_state (oal_void)
{
    hal_to_dmac_device_stru     *pst_hal_device;
    mac_device_stru             *pst_device;
    oal_uint32                   ul_is_netbuf_empty;
    extern frw_event_mem_stru   *g_pst_first_rx_event;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device      = mac_res_get_dev(0);

    pst_hal_device  = pst_device->pst_device_stru;

    if(g_pst_first_rx_event)
    {
        FRW_EVENT_FREE(g_pst_first_rx_event);
        g_pst_first_rx_event = OAL_PTR_NULL;
    }

    hcc_slave_clean();

    hal_rx_destroy_dscr_queue(pst_hal_device,OAL_TRUE);

    /* 检查除oam子池外的三个netbuf子池,深睡前应该这三个子池的netbuf内存都已经释放 */
    ul_is_netbuf_empty = oal_mem_is_netbuf_empty();

    if (ul_is_netbuf_empty != OAL_TRUE)
    {
        OAM_ERROR_LOG0(0,OAM_SF_PWR,"{dmac_psm_clean_state::before deep sleep netbuf is not empty.}");
        OAL_MEM_INFO_PRINT(OAL_MEM_POOL_ID_NETBUF);
    }

    return OAL_SUCC;
}


oal_uint8 dmac_psm_save_ps_state (oal_void)
{
    hal_to_dmac_device_stru *pst_hal_device;
    mac_device_stru         *pst_device;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device  = mac_res_get_dev(0);

    pst_hal_device  = pst_device->pst_device_stru;

    dmac_psm_save_tcm(pst_hal_device);

#ifdef _PRE_WLAN_FEATURE_EDCA_OPT
    dmac_psm_save_dutycycle(pst_device);
#endif
    dmac_psm_save_mac_statistics_data(pst_device);

    /* 保存PHY逻辑与寄存器 */
    //dmac_psm_save_phy_hw_reg(pst_hal_device);

   /* 保存MAC逻辑与寄存器 */
    //dmac_psm_save_mac_hw_reg(pst_hal_device);

    return OAL_TRUE;


}


oal_void dmac_psm_init_netbuf_pool(oal_void)
{
    oal_uint32 ul_atcm_save = 0;

    ul_atcm_save = oal_mem_is_atcm_need_save();

    if(OAL_TRUE == ul_atcm_save)
    {
        oal_mem_do_create_netbuf_subpool(OAL_MEM_NETBUF_POOL_ID_LARGE_PKT);

        oal_mem_clear_atcm_need_save_flag();
    }
}


oal_void  dmac_psm_recover_mac_reg(hal_to_dmac_device_stru  *pst_hal_device)
{
    hal_reset_reg_restore(pst_hal_device, HAL_RESET_HW_TYPE_MAC);
}


oal_void dmac_psm_recover_tcm (hal_to_dmac_device_stru  *pst_hal_device)
{
    hal_reset_reg_restore(pst_hal_device, HAL_RESET_HW_TYPE_TCM);
}


oal_void dmac_psm_recover_primary_channel(mac_device_stru *pst_device)
{
    hal_to_dmac_device_stru         *pst_h2d_device;
    hal_wifi_channel_status_stru    *pst_wifi_channel_status;

    pst_h2d_device = pst_device->pst_device_stru;

    pst_wifi_channel_status = &(pst_h2d_device->st_wifi_channel_status);

    /* 设置带宽 */
    hal_set_bandwidth_mode(pst_device->pst_device_stru, pst_wifi_channel_status->uc_bandwidth);

    hal_set_primary_channel(pst_device->pst_device_stru,
                   pst_wifi_channel_status->uc_chan_number,
                   pst_wifi_channel_status->uc_band,
                   pst_wifi_channel_status->uc_idx,
                   pst_wifi_channel_status->uc_bandwidth);


}


oal_uint8  dmac_psm_recover_powerdown(oal_uint8 uc_dmach0,oal_uint8 uc_dmach1,oal_uint8 uc_dmach2)
{
    hal_to_dmac_device_stru        *pst_hal_device;
    mac_device_stru                *pst_device;
    //oal_uint32                      ul_ret  = OAL_SUCC;
    hal_error_state_stru            st_error_state;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device      = mac_res_get_dev(0);//和平台沟通，目前先写死是device id，后面有需求再传入参
    pst_hal_device  = pst_device->pst_device_stru;


    /*和TSF同步相关的BANK1寄存器恢复*/
    dmac_psm_recover_mac_reg(pst_hal_device);

    /*tsf开始同步*/
    PM_WLAN_Tsf_Aon_to_Inner_start();

    //dmac_psm_recover_phy_reg(pst_hal_device);

    /* TCM 恢复*/
    dmac_psm_recover_tcm(pst_hal_device);

    //restart移动至paldo 开启处
    frw_timer_time_fix();

    DMAWaitChannelDone(uc_dmach0);

    dmac_psm_init_netbuf_pool();

    hal_rx_init_dscr_queue(pst_hal_device,OAL_FALSE);

     /*等待tsf同步完成*/
    PM_WLAN_Aon_Tsf_Sync();

    DMAWaitChannelDone(uc_dmach1);

    //DMAWaitChannelDone(uc_dmach2);


    /*避免复位过程中接收描述符队列异常，重新初始化接收描述符队列*/
    //hal_rx_destroy_dscr_queue(pst_hal_device,OAL_FALSE);

    dmac_psm_sync_tsf_to_ap();

    hal_irq_init((hal_device_stru*)pst_hal_device);


    hal_set_machw_rx_buff_addr(pst_hal_device, (oal_uint32)HAL_RX_DSCR_GET_REAL_ADDR(pst_hal_device->ast_rx_dscr_queue[HAL_RX_DSCR_NORMAL_PRI_QUEUE].pul_element_head), HAL_RX_DSCR_NORMAL_PRI_QUEUE);
    hal_set_machw_rx_buff_addr(pst_hal_device, (oal_uint32)HAL_RX_DSCR_GET_REAL_ADDR(pst_hal_device->ast_rx_dscr_queue[HAL_RX_DSCR_HIGH_PRI_QUEUE].pul_element_head), HAL_RX_DSCR_HIGH_PRI_QUEUE);
    hal_set_machw_rx_buff_addr(pst_hal_device, (oal_uint32)HAL_RX_DSCR_GET_REAL_ADDR(pst_hal_device->ast_rx_dscr_queue[HAL_RX_DSCR_SMALL_QUEUE].pul_element_head), HAL_RX_DSCR_SMALL_QUEUE);

    /* 清中断状态 */
    st_error_state.ul_error1_val = 0xFFFFFFFF;
    st_error_state.ul_error2_val = 0xFFFFFFFF;
    hal_clear_mac_error_int_status(pst_hal_device, &st_error_state);
    hal_clear_mac_int_status(pst_hal_device, 0xffffffff);

    /* SDIO 重新初始化 */
    hcc_slave_reinit();

    return OAL_TRUE;

}



oal_void  dmac_psm_recover_start_dma(oal_uint8 *puc_ch0,oal_uint8 *puc_ch1,oal_uint8 *puc_ch2)
{
    hal_to_dmac_device_stru        *pst_hal_device;
    mac_device_stru                *pst_device;

    pst_device      = mac_res_get_dev(0);
    pst_hal_device  = pst_device->pst_device_stru;

    hal_reset_reg_dma_restore(pst_hal_device,puc_ch0,puc_ch1,puc_ch2);

}


oal_uint8 dmac_psm_recover_no_powerdown (oal_void)
{
    hal_to_dmac_device_stru        *pst_hal_device;
    mac_device_stru                *pst_device;
    hal_error_state_stru            st_error_state;
    //oal_uint32                      ul_ret;

    pst_device      = mac_res_get_dev(0);//和平台沟通，目前先写死是device id，后面有需求再传入参
    pst_hal_device  = pst_device->pst_device_stru;


    PM_WLAN_Tsf_Aon_to_Inner_start();

    //restart移动至paldo 开启处
    frw_timer_time_fix();

    /* g_st_netbuf_pool不下电, 重新初始化内存池时需清零 */
    //OAL_MEMZERO(&g_st_netbuf_pool, OAL_SIZEOF(g_st_netbuf_pool));

    //dmac_psm_init_netbuf_pool();

    /*避免复位过程中接收描述符队列异常，重新初始化接收描述符队列*/
    //hal_rx_destroy_dscr_queue(pst_hal_device,OAL_FALSE);

    /* 低功耗不下电唤醒后清除rx中断,防止rx中断里保留上一次睡前的值造成rx new */
    hal_psm_clear_mac_rx_isr(pst_hal_device);

    hal_rx_init_dscr_queue(pst_hal_device,OAL_TRUE);


     /* 清中断状态 */
    st_error_state.ul_error1_val = 0xFFFFFFFF;
    st_error_state.ul_error2_val = 0xFFFFFFFF;
    hal_clear_mac_error_int_status(pst_hal_device, &st_error_state);
    hal_clear_mac_int_status(pst_hal_device, 0xffffffff);

    /* SDIO 重新初始化 */
    hcc_slave_reinit();

    /*等待tsf同步完成*/
    PM_WLAN_Aon_Tsf_Sync();

    dmac_psm_sync_tsf_to_ap();

    return OAL_TRUE;

}


oal_uint8 dmac_psm_cbb_stopwork (oal_void)
{
    hal_to_dmac_device_stru     *pst_hal_device;
    mac_device_stru             *pst_device;

    pst_device      = mac_res_get_dev(0);

    pst_hal_device  = pst_device->pst_device_stru;

    /* 挂起硬件发送 目前无此功能 */
    //hal_set_machw_tx_suspend(pst_hal_device);

    /* 关闭pa */
    hal_disable_machw_phy_and_pa(pst_hal_device);

    return OAL_TRUE;
}


oal_void dmac_psm_rf_sleep(oal_uint8 uc_restore_reg)
{
    hal_to_dmac_device_stru     *pst_hal_device;
    mac_device_stru             *pst_device;

    pst_device      = mac_res_get_dev(0);

    pst_hal_device  = pst_device->pst_device_stru;

    hal_psm_rf_sleep(pst_hal_device, uc_restore_reg);

    return;
}

oal_void dmac_psm_rf_awake (oal_uint8 uc_restore_reg)
{
    hal_to_dmac_device_stru     *pst_hal_device;
    mac_device_stru             *pst_device;

    /*低功耗前面的流程已检查过合法性，不需要重复检查*/
    pst_device      = mac_res_get_dev(0);

    pst_hal_device  = pst_device->pst_device_stru;


    hal_psm_rf_awake(pst_hal_device,uc_restore_reg);

    dmac_psm_recover_primary_channel(pst_device);

    return;
}




#endif

#ifdef __cplusplus
    #if __cplusplus
        }
    #endif
#endif

