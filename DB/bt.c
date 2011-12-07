/* B-tree Implementation.
 *
 * Implements in memory b-tree tables with insert/del/replace/find/ ops

AGPL License

Copyright (c) 2011 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

   This file is part of ALCHEMY_DATABASE

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <strings.h>

#include "redis.h" /* defines REDIS_BTREE */

#include "btree.h"
#include "bt.h"
#include "bt_iterator.h"
#include "index.h"
#include "query.h"
#include "stream.h"
#include "common.h"

extern r_tbl_t *Tbl;
extern r_ind_t *Index;

bt *createUUBT(int num, uchar btype) {                //printf("createUUBT\n");
    bts_t bts;
    bts.ktype = COL_TYPE_INT;
    bts.btype = btype;
    bts.ksize = UU_SIZE;
    bts.bflag = BTFLAG_AUTO_INC + BTFLAG_UINT_UINT;
    bts.num   = num;
    return bt_create(uuCmp, TRANS_ONE, &bts);
}
bt *createULBT(int num, uchar btype) {                //printf("createULBT\n");
    bts_t bts;
    bts.ktype = COL_TYPE_INT;
    bts.btype = btype;
    bts.ksize = UL_SIZE;
    bts.bflag = BTFLAG_AUTO_INC + BTFLAG_UINT_ULONG;
    bts.num   = num;
    return bt_create(ulCmp, TRANS_ONE, &bts);
}
bt *createLUBT(int num, uchar btype) {                //printf("createLUBT\n");
    bts_t bts;
    bts.ktype = COL_TYPE_LONG;
    bts.btype = btype;
    bts.ksize = LU_SIZE;
    bts.bflag = BTFLAG_AUTO_INC + BTFLAG_ULONG_UINT;
    bts.num   = num;
    return bt_create(luCmp, TRANS_ONE, &bts);
}
bt *createLLBT(int num, uchar btype) {                //printf("createLLBT\n");
    bts_t bts;
    bts.ktype = COL_TYPE_LONG;
    bts.btype = btype;
    bts.ksize = LL_SIZE;
    bts.bflag = BTFLAG_AUTO_INC + BTFLAG_ULONG_ULONG;
    bts.num   = num;
    return bt_create(llCmp, TRANS_ONE, &bts);
}
static bt *createOBT(uchar ktype, uchar vtype, int tmatch, uchar btype) {
    if        (C_IS_I(ktype)) {
        if (C_IS_I(vtype)) return createUUBT(tmatch, btype);
        if (C_IS_L(vtype)) return createULBT(tmatch, btype);
    } else if (C_IS_L(ktype)) {
        if (C_IS_I(vtype)) return createLUBT(tmatch, btype);
        if (C_IS_L(vtype)) return createLLBT(tmatch, btype);
    }
    return NULL;
}
#define ASSIGN_CMP(ktype)   C_IS_I(ktype) ? btIntCmp   : \
                          ( C_IS_L(ktype) ? btLongCmp  : \
                          ( C_IS_F(ktype) ? btFloatCmp : \
                          /* STRING */      btTextCmp))

bt *createDBT(uchar ktype, int tmatch) {
    r_tbl_t *rt = &Tbl[tmatch];
    if (rt->col_count == 2) {
        bt *obtr = createOBT(ktype, rt->col[1].type, tmatch, BTREE_TABLE);
        if (obtr) return obtr;
    }
    bts_t bts;
    bts.ktype    = ktype;
    bts.btype    = BTREE_TABLE;
    bts.ksize    = VOIDSIZE;
    bts.bflag    = C_IS_NUM(ktype) ? BTFLAG_AUTO_INC : BTFLAG_NONE;
    bts.num      = tmatch;
    bt_cmp_t cmp = ASSIGN_CMP(ktype);
    return bt_create(cmp, TRANS_ONE, &bts);
}
bt *createIBT(uchar ktype, int imatch, uchar btype) {
    bt_cmp_t cmp; bts_t bts;
    bts.ktype    = ktype;
    bts.btype    = btype;
    bts.num      = imatch;
    if        C_IS_I(ktype) { /* NOTE: under the covers: ULBT */
        bts.ksize = UL_SIZE;
        bts.bflag = BTFLAG_UINT_ULONG  + BTFLAG_UINT_PTR;
        cmp       = ulCmp;
    } else if C_IS_L(ktype) { /* NOTE: under the covers: LLBT */
        bts.ksize = LL_SIZE;
        bts.bflag = BTFLAG_ULONG_ULONG + BTFLAG_ULONG_PTR;
        cmp       = llCmp;
    } else { /* STRING or FLOAT */
        bts.ksize = VOIDSIZE;
        bts.bflag = BTFLAG_NONE;
        cmp       = ASSIGN_CMP(ktype);
    }
    return bt_create(cmp, TRANS_ONE, &bts);
}
bt *createMCI_MIDBT(uchar ktype, int imatch) {
    return createIBT(ktype, imatch, BTREE_MCI_MID);
}
bt *createMCIndexBT(list *clist, int imatch) {
    listNode *ln    = listFirst(clist);
    r_tbl_t  *rt    = &Tbl[Index[imatch].table];
    uchar     ktype = rt->col[(int)(long)ln->value].type;
    return createIBT(ktype, imatch, BTREE_MCI);
}
bt *createIndexNode(uchar ktype, uchar obctype) {                /* INODE_BT */
    if (obctype != COL_TYPE_NONE) {
        bt *btr       =  createOBT(obctype, ktype, -1, BTREE_INODE);
        btr->s.bflag |= BTFLAG_OBC; // will fail INODE(btr) -> OBC is different
        // NOTE: BTFLAG_*_PTR used as the RAW [VALUE] is pulled out in
        //       parseStream() and passed to nodeBT_Op()
        //       [UU,UL,LU,LL] are passed as [KEY,VALUE] to getRawCol()
        btr->s.bflag |= (C_IS_I(obctype)) ? BTFLAG_UINT_PTR :
                      /* C_IS_L(ktype) */   BTFLAG_ULONG_PTR;
        return btr;
    }
    bt_cmp_t cmp; bts_t bts;
    bts.ktype = ktype;
    bts.btype = BTREE_INODE;
    bts.num   = -1;
    if (C_IS_I(ktype)) {
        cmp       = uintCmp;
        bts.ksize = UINTSIZE;
        bts.bflag = BTFLAG_AUTO_INC;
    } else if (C_IS_L(ktype)) {
        cmp       = ulongCmp;
        bts.ksize = ULONGSIZE;
        bts.bflag = BTFLAG_AUTO_INC;
    } else {
        cmp       = ASSIGN_CMP(ktype);
        bts.ksize = VOIDSIZE;
        bts.bflag = BTFLAG_NONE;
    }
    return bt_create(cmp, TRANS_ONE, &bts);
}
bt *createIndexBT(uchar ktype, int imatch) {
    return createIBT(ktype, imatch, BTREE_INDEX);
}

/* ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE ABSTRACT-BTREE */
#define DEBUG_REP_NSIZE         printf("osize: %d nsize: %d\n", osize, nsize);
#define DEBUG_REP_SAMESIZE      printf("abt_replace: SAME_SIZE\n");
#define DEBUG_REP_REALLOC       printf("o2: %p o: %p\n", o2stream, *ostream);
#define DEBUG_REP_REALLOC_OVRWR printf("abt_replace: REALLOC OVERWRITE\n");
#define DEBUG_REP_RELOC_NEW     printf("abt_replace: REALLOC -> new \n");

static int abt_replace(bt *btr, aobj *akey, void *val) {
    uint32 ssize; DECLARE_BT_KEY(akey, 0)
    uchar  *nstream = createStream(btr, val, btkey, ksize, &ssize);
    if NORM_BT(btr) {
        uchar  **ostream = (uchar **)bt_find_loc(btr, btkey);
        destroyBTKey(btkey, med);                        /* FREED 026 */
        uint32   osize   = getStreamMallocSize(btr, *ostream);
        uint32   nsize   = getStreamMallocSize(btr,  nstream); //DEBUG_REP_NSIZE
        if (osize == nsize) { /* if ROW size doesnt change, just overwrite */
            memcpy(*ostream, nstream, osize);             //DEBUG_REP_SAMESIZE
            destroyStream(btr, nstream);
        } else {              /* try to realloc (may re-use memory -> WIN) */
            bt_decr_dsize(btr, osize); bt_incr_dsize(btr, nsize);
            void *o2stream = realloc(*ostream, nsize);      //DEBUG_REP_REALLOC
            if (o2stream == *ostream) { /* realloc overwrite -> WIN */
                memcpy(*ostream, nstream, nsize);     //DEBUG_REP_REALLOC_OVRWR
                destroyStream(btr, nstream);
            } else {          /* new pointer, copy nstream, replace in BT */
                *ostream = nstream;/* REPLACE ptr in BT */ //DEBUG_REP_RELOC_NEW
            }
        }
    } else {
        uchar *dstream = bt_replace(btr, btkey, nstream);
        destroyBTKey(btkey, med);                        /* FREED 026 */
        destroyStream(btr, dstream);
    }
    return ssize;
}
static void *abt_find(bt *btr, aobj *akey) {
    DECLARE_BT_KEY(akey, 0)
    uchar *stream = bt_find(btr, btkey, akey);
    destroyBTKey(btkey, med);                            /* FREED 026 */
    return parseStream(stream, btr);
}
static bool abt_exist(bt *btr, aobj *akey) { //NOTE: Evicted Indexes are NULL
    DECLARE_BT_KEY(akey, 0)
    bool ret = bt_exist(btr, btkey, akey);
    destroyBTKey(btkey, med);                            /* FREED 026 */
    return ret;
}
static dwm_t abt_find_d(bt *btr, aobj *akey) { //NOTE: use for dirty tables
    dwm_t dwme; bzero(&dwme, sizeof(dwm_t));
    DECLARE_BT_KEY(akey, dwme)
    dwm_t  dwm = findnodekey(btr, btr->root, btkey, akey);
    destroyBTKey(btkey, med);                            // FREED 026
    dwm.k      = parseStream(dwm.k, btr);
    return dwm;
}
static bool abt_del(bt *btr, aobj *akey) { // DELETE the row
    DECLARE_BT_KEY(akey, 0)
    dwd_t  dwd    = bt_delete(btr, btkey, akey);         /* FREED 028 */
    if (dwd.ngost) { // replace MISS w/ GHOST,abt abstraction needed -- ECASE:1
                     // TODO: this should be done in bt_code.c: remove_key
        aobj gpk; initAobjFromLong(&gpk, dwd.ngost, btr->s.ktype);
        uint32 ssize; DECLARE_BT_KEY(&gpk, 0)
        char *stream = createStream(btr, NULL, btkey, ksize, &ssize);//DEST 027
        bt_insert_ghost(btr, btkey, &gpk, stream, dwd.dr);       // FREE ME 028
        destroyBTKey(btkey, med);                                  // FREED 026
    }
    uchar *stream = dwd.k;
    destroyBTKey(btkey, med);                            /* FREED 026 */
    return destroyStream(btr, stream);                   /* DESTROYED 027 */
}
static void abt_del_d(bt *btr, aobj *akey) { // GHOST the row
    uint32 ssize; DECLARE_BT_KEY(akey, )
    char   *stream = createStream(btr, NULL, btkey, ksize, &ssize); // DEST 027
    bt_delete_d(btr, btkey, akey, stream);
    destroyBTKey(btkey, med);
}
static bool abt_evict(bt *btr, aobj *akey) {
    DECLARE_BT_KEY(akey, 0)
    uchar * stream = bt_evict(btr, btkey);
    destroyBTKey(btkey, med);                            // FREED 026
    return destroyStream(btr, stream);                   // DESTROYED 027
}
static uint32 abt_get_dr(bt *btr, aobj *akey) {
    DECLARE_BT_KEY(akey, 0)
    uint32 dr = bt_get_dr(btr, btkey, akey); destroyBTKey(btkey, med);
    return dr;
}
static bool abt_decr_dr_pk(bt *btr, aobj *akey, uint32 by) {
    DECLARE_BT_KEY(akey, 0) return bt_decr_dr_pk(btr, btkey, akey, by);

}
static uint32 abt_insert(bt *btr, aobj *akey, void *val) {
#ifndef TEST_WITH_TRANS_ONE_ONLY
    if (btr->numkeys == TRANS_ONE_MAX) btr = abt_resize(btr, TRANS_TWO);
#endif
    uint32 ssize; DECLARE_BT_KEY(akey, 0)
    char   *stream = createStream(btr, val, btkey, ksize, &ssize); /*DEST 027*/
    destroyBTKey(btkey, med);                            /* FREED 026 */
    bt_insert(btr, stream);                              /* FREE ME 028 */
    return ssize;
}
bt *abt_resize(bt *obtr, uchar trans) {              // printf("abt_resize\n");
     bts_t bts;
     memcpy(&bts, &obtr->s, sizeof(bts_t)); /* copy flags */
     bt *nbtr    = bt_create(obtr->cmp, trans, &bts);
     nbtr->dsize = obtr->dsize;
    if (obtr->root) {
        bt_to_bt_insert(nbtr, obtr, obtr->root); /* 1.) copy from old to new */
        bt_release(obtr, obtr->root);            /* 2.) release old */
        memcpy(obtr, nbtr, sizeof(bt));          /* 3.) overwrite old w/ new */
        free(nbtr);                              /* 4.) free new */
    } //bt_dump_info(obtr, obtr->ktype);
    return obtr;
}

/* DATA DATA DATA DATA DATA DATA DATA DATA DATA DATA DATA DATA DATA DATA */
int   btAdd    (bt *btr, aobj *apk, void *val) {
                                      return abt_insert (btr, apk, val); }
int   btReplace(bt *btr, aobj *apk, void *val) {
                                      return abt_replace(btr, apk, val); }
void *btFind   (bt *btr, aobj *apk) { return abt_find   (btr, apk); }
int   btDelete (bt *btr, aobj *apk) { return abt_del    (btr, apk); }

// DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY DIRTY
dwm_t btFindD  (bt *btr, aobj *apk) { return abt_find_d(btr, apk); }
int   btDeleteD(bt *btr, aobj *apk) { abt_del_d (btr, apk); return 1; }
//NOTE: btFindD() must precede btEvict()
bool  btEvict  (bt *btr, aobj *apk) { return abt_evict (btr, apk); }

/* INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX INDEX */
void  btIndAdd   (bt *ibtr, aobj *ikey, bt *nbtr) {
                                               abt_insert(ibtr, ikey, nbtr);
}
bt   *btIndFind  (bt *ibtr, aobj *ikey) { return abt_find (ibtr, ikey); }
bool  btIndExist (bt *ibtr, aobj *ikey) { return abt_exist(ibtr, ikey); }
int   btIndDelete(bt *ibtr, aobj *ikey) {
                                      abt_del(ibtr, ikey); return ibtr->numkeys;
}
void btIndNull   (bt *ibtr, aobj *ikey) { abt_replace(ibtr, ikey, NULL); }

/* INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE INDEX_NODE */
#define DEBUG_INODE_ADD                                                   \
    printf("btIndNodeAdd: apk : "); dumpAobj(printf, apk);                \
    if (ocol) { printf("btIndNodeAdd: ocol: "); dumpAobj(printf, ocol); }
#define DEBUG_INODE_EVICT \
    printf("btIndNodeEvict: nkeys: %d apk: %p: ",       \
            nbtr->numkeys, apk); dumpAobj(printf, apk);

void *btIndNodeFind(bt *nbtr, aobj *apk) { return abt_find(nbtr, apk); }
bool  btIndNodeAdd (cli *c, bt *nbtr, aobj *apk, aobj *ocol) { DEBUG_INODE_ADD
    if (ocol) {
        if (abt_find(nbtr, ocol)) {
            if (c) addReply(c, shared.obindexviol); return 0;
        }
        if      C_IS_I(apk->type) abt_insert(nbtr, ocol, VOIDINT apk->i);
        else /* C_IS_L */         abt_insert(nbtr, ocol, (void *)apk->l);
    } else abt_insert(nbtr, apk, NULL);
    return 1;
}
int  btIndNodeDelete(bt *nbtr, aobj *apk, aobj *ocol) {
    abt_del  (nbtr, ocol ? ocol : apk); return nbtr->numkeys;
}
int  btIndNodeEvict(bt *nbtr, aobj *apk, aobj *ocol) {       DEBUG_INODE_EVICT
    abt_evict(nbtr, ocol ? ocol : apk); return nbtr->numkeys;
}

// HELPER HELPER HELPER HELPER HELPER HELPER HELPER HELPER HELPER HELPER
uint32  btGetDR  (bt *btr, aobj *akey) { return abt_get_dr(btr, akey); }
aobj   *btGetNext(bt *btr, aobj *akey) {
printf("btGetNext\n");
    aobj     aH; if (!assignMaxKey(btr, &aH))                   return NULL;
    aobj    *rkey = NULL;
    btSIter *bi   = btGetRangeIter(btr, akey, &aH, 1); if (!bi) goto btgn_e;
    btEntry *be   = btRangeNext(bi, 1);                if (!be) goto btgn_e;
    be            = btRangeNext(bi, 1);                if (!be) goto btgn_e;
    rkey          = be->key;

btgn_e:
    btReleaseRangeIterator(bi);
    return rkey;
}
bool btDecrDR_PK(bt *btr, aobj *akey, uint32 by) {
    printf("btDecrDR_PK: by: %u, key: ", by); dumpAobj(printf, akey);
    return abt_decr_dr_pk(btr, akey, by);
}
