#ifndef HV_W03_LAYOUT_H
#define HV_W03_LAYOUT_H
#ifndef HV_W02
#error W03 must use the cold W02 firmware path
#endif
enum { W03DescriptorGpa=0x00fff000u, W03DescriptorTag=0x33424d57u,
       W03FileGpa=0x01000000u, W03FileCapacity=0x01000000u };
_Static_assert(W03_FILE_SIZE>0 && W03_FILE_SIZE<=W03FileCapacity,"fixed source window");
_Static_assert(W03_SECTION_ALIGN>=4096 && !(W03_SECTION_ALIGN&(W03_SECTION_ALIGN-1)) &&
               W03_IMAGE_SIZE && !(W03_IMAGE_SIZE&(W03_SECTION_ALIGN-1)) &&
               W03_IMAGE_SIZE<=96u*1024*1024,"private PE alignment and extent");
_Static_assert(W03_TEXT_LEN>=4 && W03_TEXT_LEN<=W03_FILE_SIZE && W03_TEXT_LEN<=W03_IMAGE_SIZE &&
               W03_TEXT_FILE_OFFSET<=W03_FILE_SIZE-W03_TEXT_LEN &&
               !(W03_TEXT_RVA&(W03_SECTION_ALIGN-1)) && W03_TEXT_RVA<=W03_IMAGE_SIZE-W03_TEXT_LEN &&
               W03_ENTRY_RVA>=W03_TEXT_RVA && W03_ENTRY_RVA-W03_TEXT_RVA<=W03_TEXT_LEN-4 &&
               (W03_ENTRY_RVA&4095)<=4096-4,"pinned, initialized, single-page entry");
#endif
