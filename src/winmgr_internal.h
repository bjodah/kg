#ifndef KG_WINMGR_INTERNAL_H
#define KG_WINMGR_INTERNAL_H

/* Private seam shared by winmgr.c and winconfig.c.  A bulk replacement
 * checks the whole identity demand first, then claims each prepared slot. */
int win_identities_available(int count);
int win_claim_slot_for_layout(int slot);

#endif /* KG_WINMGR_INTERNAL_H */
