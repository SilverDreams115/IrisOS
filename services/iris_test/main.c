/*
 * main.c — the iris_test suite's entry point and its running order.
 *
 * Spawned by init after the healthy path is established.  Sets up the two
 * second-level CNodes the suite fabricates into, calls every test in order,
 * and prints the marker smoke-runtime greps for:
 *   [IRIS][TEST] SUITE PASS N/N
 * Exits 0 on a full pass, 1 on any failure.
 *
 * The tests themselves live in it_tNNN_tNNN.c, one file per range of test
 * numbers; the shared helpers are in it_base.c and the interface between all
 * of them is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
/* ── Entry point ────────────────────────────────────────────────────────── */

void iris_test_main(handle_id_t rbx_unused) {
    /* Phase 13 (Track I): the spawn/authority cap arrives as the
     * IRIS_CPTR_PROC_CONTROL (slot 6) pre-start mint — no bootstrap KChannel.
     * SYS_CAP_CREATE_IOPORT resolves it by CPtr via the device-cap dual
     * resolver (the serial KIoPort for test output).  svc_loader passes
     * RBX = 0, so this argument is not a handle. */
    (void)rbx_unused;

    /* Second-level CNode for the objects the suite fabricates and HOLDS.  The
     * root CNode is full, so this is the only place they can live — and it is
     * only addressable because handles moved out of the low CPtr range. */
    (void)it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)), (long)((uint64_t)IT_OBJ_CNODE_SLOT << 32), 256);

    /* Stage 7 Step 13: and one for the child threads the suite supervises.
     * Separate from the objects CNode because it must hold as many entries as
     * the suite holds children — 48 in T240 — which is more leaves than the
     * objects CNode has spare. */
    (void)it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)), (long)((uint64_t)IT_CHILD_CN_SLOT << 32), (long)IT_CHILD_CN_SLOTS);

    {
        /* Phase S4: device caps are published into a CSpace slot as MDB
         * children of the authorising slot; the result is a CPtr, and
         * SYS_IOPORT_IN/OUT resolve it through their CSpace leg.
         * Stage 5 Step 2: the authorising slot holds the ioport CONTROL
         * capability — printing test output no longer needs the authority to
         * spawn processes. */
        if (it_ioport_create((long)IRIS_CPTR_IOPORT_CONTROL, 0x3F8, 8, (long)IT_SERIAL_SLOT) == 0)
            g_serial_h = (handle_id_t)IT_SERIAL_SLOT;
    }

    it_serial_write("[IRIS][TEST] start\n");

    /*
     * D-4: the main thread's own IPC buffer, before any test runs.
     *
     * The buffer lands
     * at IRIS_IPC_BUFFER_VA, and every thread this suite creates gets the next
     * page after it (it_thread_ipc_buffer).  Best-effort while the row is
     * migrating — a failure leaves the static fallback, which is the path the
     * kernel is about to stop having.
     */
    {
        /* Through the same helper the suite's other threads use, so the frame
         * lands in the dedicated CNode and NO root slot is consumed.
         *
         * Consuming two was the first attempt, and it broke four tests that
         * measure when the root CSpace fills — an assertion about how many
         * slots are free is an assertion this had no business changing. */
        long tcb_self = it_own_tcb_derived();
        if (it_setup_self_vspace() && tcb_self >= 0) {
            long va = it_thread_ipc_buffer(tcb_self);
            if (va > 0) g_ep_io_buf = (uint8_t *)(uintptr_t)va;
            it_slot_delete((uint32_t)tcb_self);
        }
    }

    /* Run all tests */
    test_t001();
    test_t002();
    test_t003();
    /* T004-T007 retired (Phase 13/Track F): the KChannel send/recv, NB-recv,
     * recv-timeout and seal/close semantics are covered by the endpoint /
     * notification equivalents — T015 (EP_SEND/RECV), T014 (EP_NB_RECV empty
     * → WOULD_BLOCK), T016 (EP_CALL/REPLY), T019 (endpoint close wakes a
     * blocked recv) and T010 (NOTIFY_WAIT_TIMEOUT → TIMED_OUT). */
    test_t008();
    test_t009();
    test_t010();
    test_t013();
    test_t014();
    test_t015();
    test_t016();
    test_t018();
    test_t019();
    test_t020();
    test_t021();
    test_t022();
    test_t023();
    test_t024();
    test_t025();
    test_t026();
    test_t027();
    test_t028();
    test_t029();
    test_t030();
    test_t031();
    test_t032();
    test_t033();
    test_t034();
    test_t035();
    test_t036();
    test_t037();
    test_t038();
    test_t039();
    test_t040();
    test_t041();
    test_t042();
    test_t043();
    test_t044();
    test_t045();
    test_t047();
    test_t048();
    test_t049();
    test_t050();
    test_t051();
    test_t052();
    test_t053();
    test_t054();
    test_t055();
    test_t056();
    test_t057();
    test_t058();
    test_t059();
    test_t060();
    test_t061();
    test_t062();
    test_t063();
    test_t064();
    test_t065();
    test_t066();
    test_t067();
    test_t068();
    test_t069();
    test_t070();
    test_t071();
    test_t072();
    test_t073();
    test_t074();
    test_t075();
    test_t076();
    test_t077();
    test_t078();
    test_t079();
    test_t080();
    test_t081();
    test_t082();
    test_t083();
    test_t084();
    test_t085();
    test_t086();
    test_t087();
    test_t088();
    test_t089();
    test_t090();
    test_t091();
    test_t092();
    test_t093();
    test_t094();
    test_t095();
    test_t096();
    test_t097();
    test_t098();
    test_t099();
    test_t100();
    test_t101();
    test_t102();
    test_t103();
    test_t104();
    test_t105();
    test_t106();
    test_t107();
    test_t108();
    test_t109();
    test_t110();
    test_t111();
    test_t112();
    test_t113();
    test_t114();
    test_t115();
    test_t116();
    test_t117();
    test_t118();
    test_t119();
    test_t120();
    test_t121();
    test_t122();
    test_t123();
    test_t124();
    test_t125();
    test_t126();
    test_t127();
    test_t128();
    test_t129();
    test_t130();
    test_t131();
    test_t132();
    test_t133();
    test_t134();
    test_t135();
    test_t136();
    test_t137();
    test_t138();
    test_t139();
    test_t140();
    test_t141();
    test_t142();
    test_t143();
    test_t144();
    test_t145();
    test_t146();
    test_t147();
    test_t148();
    test_t149();
    test_t150();
    test_t151();
    test_t152();
    test_t153();
    test_t154();
    test_t155();
    test_t156();
    test_t157();
    test_t158();
    test_t159();
    test_t160();
    test_t161();
    test_t162();
    test_t163();
    test_t164();
    test_t165();
    test_t166();
    test_t167();
    test_t168();
    test_t169();
    test_t170();
    test_t171();
    test_t172();
    test_t173();
    test_t174();
    test_t175();
    test_t176();
    test_t177();
    test_t178();
    test_t179();
    test_t180();
    test_t181();
    test_t182();
    test_t183();
    test_t184();
    test_t185();
    test_t186();
    test_t187();
    test_t188();
    test_t189();
    test_t190();
    test_t191();
    test_t192();
    test_t193();
    test_t194();
    test_t195();
    test_t196();
    test_t197();
    test_t198();
    test_t199();
    test_t200();
    test_t201();
    test_t202();
    test_t203();
    test_t204();
    test_t205();
    test_t206();
    test_t207();
    test_t208();
    test_t209();
    test_t210();
    test_t211();
    test_t212();
    test_t213();
    test_t214();
    test_t215();
    test_t216();
    test_t217();
    test_t218();
    test_t219();
    test_t220();
    test_t221();
    test_t222();
    test_t223();
    test_t224();
    test_t225();
    test_t226();
    test_t227();
    test_t228();
    test_t229();
    test_t230();
    test_t231();
    test_t232();
    test_t233();
    test_t234();
    test_t235();
    test_t236();
    test_t237();
    test_t238();
    test_t239();
    test_t240();
    test_t244();
    test_t245();
    test_t248();
    test_t249();
    test_t250();

    /* Phase S1 — seL4 Architectural Convergence suite. */
    test_t251();
    test_t252();
    test_t253();
    test_t254();
    test_t255();
    test_t256();
    test_t257();
    test_t258();
    test_t259();
    test_t260();
    test_t261();
    test_t262();

    /* Phase S2 — Untyped task construction (increment 1: SchedulingContext). */
    test_t267();
    /* Phase S2 Checkpoint C.1 — versioned user-buffer ABI hardening. */
    test_t283();
    /* Phase S2 Step 0 — canonical TCB from Untyped (adversarial lifecycle). */
    test_t284();
    test_t285();
    test_t286();
    test_t287();
    /* Phase S3 — native MDB/CDT, cross-process revoke. */
    test_t288();
    test_t289();
    test_t290();
    /* T291 retired with SYS_BOOTCAP_RESTRICT (Stage 5 Step 2). */
    test_t292();
    test_t293();
    test_t294();
    test_t295();
    /* Stage 5: one capability, one authority. */
    test_t296();
    /* Stage 5: a thread is retyped, configured and started. */
    test_t297();
    /* Stage 6: the Untyped pays for its frames' headers. */
    test_t298();
    /* Stage 6: page tables are charged to a budget. */
    test_t299();
    /* Stage 6: user memory comes out of a named budget. */
    test_t300();
    /* Stage 6: a refused spawn leaves its budget untouched. */
    test_t301();
    /* Stage 6-pure: a page table is a capability the holder retypes. */
    test_t302();
    /* Stage 7: a running thread outlives every capability to it. */
    test_t303();
    test_t304();
    test_t305();
    test_t306();
    test_t307();
    test_t308();
    test_t309();
    test_t310();
    test_t311();
    test_t312();
    test_t313();
    test_t314();
    test_t315();
    test_t316();
    test_t317();
    test_t318();
    test_t319();
    test_t320();
    test_t321();
    test_t322();
    test_t323();
    test_t325();
    test_t326();
    test_t327();
    test_t328();
    test_t329();
    test_t330();
    test_t331();
    test_t332();
    test_t333();
    test_t334();
    test_t335();
    test_t336();
    test_t337();
    test_t338();
    test_t339();
    test_t324();

    /* g_svcmgr_ep_h is a CPtr slot (not a handle): nothing to close. */
    it_close(&g_vfs_ep_h);

    /* Final summary marker */
    if (g_pass == g_total) {
        it_serial_write("[IRIS][TEST] SUITE PASS ");
        it_log_num(g_pass);
        it_serial_write("/");
        it_log_num(g_total);
        it_serial_write("\n");
    } else {
        it_serial_write("[IRIS][TEST] SUITE FAIL ");
        it_log_num(g_pass);
        it_serial_write("/");
        it_log_num(g_total);
        it_serial_write("\n");
    }

    /* g_serial_h is IT_SERIAL_SLOT, a CSpace slot holding the device cap
     * SYS_CAP_CREATE_IOPORT published there — never a handle.  Closing it as
     * one was a failed call that read as cleanup; the slot goes with the
     * address space on exit. */
    it_sys1(SYS_EXIT, (long)(g_pass != g_total ? 1 : 0));
    for (;;) {}
}

