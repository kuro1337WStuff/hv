#ifndef HV_LINUX_INPUT_TRIAL_H
#define HV_LINUX_INPUT_TRIAL_H

static const char *linux_input_trial(UINT8 *base)
{
    static const struct {
        unsigned offset;
        enum LinuxInput expected;
        char bad_label[13], runs_label[13], failure[17];
    } arms[] = {
        {LinuxImageOff+LinuxImageLen-1, LinuxInputKernel, "KERNEL BAD ", "KERNEL RUN ", "KERNEL PIN TRIAL"},
        {LinuxRdOff+LinuxRdLen-1, LinuxInputInitrd, "INITRD BAD ", "INITRD RUN ", "INITRD PIN TRIAL"},
        {LinuxCmdOff, LinuxInputCmdline, "CMDLINE BAD ", "CMDLINE RUN ", "CMD PIN TRIAL"}
    };
    UINT8 *media=linux_stage(base);
    for (size_t i=0; i<sizeof(arms)/sizeof(arms[0]); ++i) {
        UINT8 *byte=media+arms[i].offset;
        UINT8 saved=*byte;
        unsigned before=linux_vmrun_count;
        enum LinuxInput bad=LinuxInputOK;
        *byte=(UINT8)(saved^1u);
        const char *error=linux_verified_admit(base,&bad);
        *byte=saved;
        unsigned runs=linux_vmrun_count-before;
        pkg_observe(arms[i].bad_label,bad);
        pkg_observe(arms[i].runs_label,runs);
        if (!error || bad!=arms[i].expected || runs!=0) return arms[i].failure;
    }
    unsigned before=linux_vmrun_count;
    enum LinuxInput bad=LinuxInputKernel;
    const char *error=linux_verified_admit(base,&bad);
    unsigned runs=linux_vmrun_count-before;
    pkg_observe("RESTORE BAD ",bad);
    pkg_observe("RESTORE RUN ",runs);
    if (error) return error;
    if (bad!=LinuxInputOK || runs!=4) return "RESTORE PIN TRIAL";
    return NULL;
}

#endif
