#pragma once

static void __cdecl TS_Ret0_cdecl_log()
{
}

static void __cdecl TS_Ret0_cdecl()
{
}

static void __stdcall TS_vt0_14h(int a1, int a2, int a3, int a4, int a5)
{
}

static int __cdecl TS_vt6_ret0()
{
    return 0;
}

static int __stdcall TS_vt8_1Ch(int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    return -1;
}

static int __cdecl TS_vt9_ret0()
{
    return 0;
}

static int __stdcall TS_vt10_04(int a1)
{
    return 0;
}

static int __cdecl TS_vt11_ret0()
{
    return 0;
}

static int __cdecl TS_vt12_ret0()
{
    return 0;
}

static int __cdecl TS_vt13_ret0()
{
    return 0;
}

static int __cdecl TS_vt14_ret0()
{
    return 0;
}

static int __cdecl TS_vt15_ret0()
{
    return 0;
}

static int __cdecl TS_vt16_ret0()
{
    return 0;
}

static int __cdecl TS_vt17_ret0()
{
    return 0;
}

static int __cdecl TS_vt18_ret0()
{
    return 0;
}

static int __cdecl TS_vt19_ret0()
{
    return 0;
}

static int __cdecl TS_vt20_ret0()
{
    return 0;
}

static int __cdecl TS_vt21_ret0()
{
    return 0;
}

static int __cdecl TS_vt22_ret0()
{
    return 0;
}

static int __cdecl TS_vt23_ret0()
{
    return 0;
}

static int __cdecl TS_vt24_ret0()
{
    return 0;
}

static int __cdecl TS_vt25_ret0()
{
    return 0;
}

static int __stdcall TS_vt26_04(int a1)
{
    // NetworkProc dispatches game packets only when this returns 1.
    return 1;
}


static void* g_vtable[] = {
    (void*)TS_vt0_14h,      // [0]  ret 14h — 5 args
    (void*)TS_Ret0_cdecl,   // [1]  ret 0
    (void*)TS_Ret0_cdecl,   // [2]  ret 0
    (void*)TS_Ret0_cdecl,   // [3]  ret 0
    (void*)TS_Ret0_cdecl,   // [4]  ret 0
    (void*)TS_Ret0_cdecl,   // [5]  ret 0
    (void*)TS_vt6_ret0,     // [6]  return 0, ret 0
    (void*)TS_Ret0_cdecl,   // [7]  ret 0
    (void*)TS_vt8_1Ch,      // [8]  return -1, ret 1Ch — 7 args
    (void*)TS_vt9_ret0,     // [9]  return 0, ret 0
    (void*)TS_vt10_04,      // [10] return 0, ret 4 — 1 arg
    (void*)TS_vt11_ret0,    // [11] return 0, ret 0
    (void*)TS_vt12_ret0,    // [12] return 0, ret 0
    (void*)TS_vt13_ret0,    // [13] return 0, ret 0
    (void*)TS_vt14_ret0,    // [14] return 0, ret 0
    (void*)TS_vt15_ret0,    // [15] return 0, ret 0
    (void*)TS_vt16_ret0,    // [16] return 0, ret 0
    (void*)TS_vt17_ret0,    // [17] return 0, ret 0
    (void*)TS_vt18_ret0,    // [18] return 0, ret 0
    (void*)TS_vt19_ret0,    // [19] return 0, ret 0
    (void*)TS_vt20_ret0,    // [20] return 0, ret 0
    (void*)TS_vt21_ret0,    // [21] return 0, ret 0
    (void*)TS_vt22_ret0,    // [22] return 0, ret 0
    (void*)TS_vt23_ret0,    // [23] return 0, ret 0
    (void*)TS_vt24_ret0,    // [24] return 0, ret 0
    (void*)TS_vt25_ret0,    // [25] return 0, ret 0
    (void*)TS_vt26_04,      // [26] return 1, ret 4 — 1 arg
};

static void** g_vtable7 = &g_vtable[10];

static void* g_tersafe_ptr3 = (void*)g_vtable;
static void* g_tersafe_ptr7 = (void*)g_vtable7;

void** CreateObj_Impl(char type)
{
    if (type == 3)
        return (void**)&g_tersafe_ptr3;
    if (type == 7)
        return (void**)&g_tersafe_ptr7;
    return nullptr;
}
