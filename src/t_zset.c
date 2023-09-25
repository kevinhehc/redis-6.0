/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*-----------------------------------------------------------------------------
 * Sorted set API
 * 排序集合API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * Zset 为有序集合，它使用两种数据结构保存同一个对象，
 * 使得可以在有序集合内用 O(log(N)) 复杂度内进行添加和删除操作。
 *
 * The elements are added to a hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view").
 *
 * 哈希表以 Redis 对象为键， score 为值。
 * Skiplist 里同样保持着 Redis 对象和 score 值的映射。
 *
 * Note that the SDS string representing the element is the same in both
 * the hash table and skiplist in order to save memory. What we do in order
 * to manage the shared SDS string more easily is to free the SDS string
 * only in zslFreeNode(). The dictionary has no value free method set.
 * So we should always remove an element from the dictionary, and later from
 * the skiplist.
 * This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 *
 * 这里的 skiplist 实现和 William Pugh 在 "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees" 里描述的差不多，只有三个地方进行了修改：
 *
 * a) this implementation allows for repeated scores.
 *    这个实现允许重复值
 * b) the comparison is not just by key (our 'score') but by satellite data.
 *    不仅对 score 进行比对，还需要对 Redis 对象里的信息进行比对
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE.
 *    每个节点都带有一个前驱指针，用于从表尾向表头迭代。
 */

#include "server.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Skiplist implementation of the low level API
 * 低级API的Skiplist实现
 *----------------------------------------------------------------------------*/

int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Create a skiplist node with the specified number of levels.
 * The SDS string 'ele' is referenced by the node after the call. 
 *
 * 创建具有指定级别数的skiplist节点。SDS字符串“ele”在调用后被节点引用。
 * */
zskiplistNode *zslCreateNode(int level, double score, sds ele) {
    // 分配层
    zskiplistNode *zn =
        zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));
    // 点数
    zn->score = score;
    // 对象
    zn->ele = ele;
    return zn;
}

/* Create a new skiplist. 
 *
 * 创建一个新的skiplist。
 * */
zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl;

    zsl = zmalloc(sizeof(*zsl));
    zsl->level = 1;
    zsl->length = 0;
    // 初始化头节点， O(1)
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    // 初始化层指针，O(1)
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->level[j].forward = NULL;
        zsl->header->level[j].span = 0;
    }
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}

/* Free the specified skiplist node. The referenced SDS string representation
 * of the element is freed too, unless node->ele is set to NULL before calling
 * this function. 
 *
 * 释放指定的skiplist节点。元素的引用SDS字符串表示也被释放，除非在调用此
 * 函数之前node->ele被设置为NULL。
 * */
void zslFreeNode(zskiplistNode *node) {
    sdsfree(node->ele);
    zfree(node);
}

/* Free a whole skiplist. 
 *
 * 释放整个跳跃表
 *
 * T = O(N)
 */
void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->level[0].forward, *next;

    zfree(zsl->header);
    // 遍历删除, O(N)
    while(node) {
        next = node->level[0].forward;
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}

/* Returns a random level for the new skiplist node we are going to create.
 * The return value of this function is between 1 and ZSKIPLIST_MAXLEVEL
 * (both inclusive), with a powerlaw-alike distribution where higher
 * levels are less likely to be returned.
 *
 * 返回一个介于 1 和 ZSKIPLIST_MAXLEVEL 之间的随机值，作为节点的层数。
 *
 * 根据幂次定律(power law)，数值越大，函数生成它的几率就越小
 *
 * T = O(N)
 * */
int zslRandomLevel(void) {
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

/* Insert a new node in the skiplist. Assumes the element does not already
 * exist (up to the caller to enforce that). The skiplist takes ownership
 * of the passed SDS string 'ele'. 
 *
 * 在skiplist中插入一个新节点。假设元素不存在（由调用方强制执行）。skip
 * list拥有传递的SDS字符串“ele”的所有权。
 * */
/*
 * 将包含给定 score 的对象 obj 添加到 skiplist 里
 *
 * T_worst = O(N), T_average = O(log N)
 */
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele) {

    // 记录寻找元素过程中，每层能到达的最右节点
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;

    // 记录寻找元素过程中，每层所跨越的节点数
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;

    serverAssert(!isnan(score));
    x = zsl->header;

    // 记录沿途访问的节点，并计数 span 等属性
    // 平均 O(log N) ，最坏 O(N)
    for (i = zsl->level-1; i >= 0; i--) {
        /* store rank that is crossed to reach the insert position 
         *
         * 交叉到达插入位置的存储列组
         * */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];
        // 右节点不为空
        while (x->level[i].forward &&
                // 右节点的 score 比给定 score 小
                (x->level[i].forward->score < score ||
                        // 右节点的 score 相同，但节点的 member 比输入 member 要小
                    (x->level[i].forward->score == score &&
                    sdscmp(x->level[i].forward->ele,ele) < 0)))
        {
            // 记录跨越了多少个元素
            rank[i] += x->level[i].span;
            // 继续向右前进
            x = x->level[i].forward;
        }
        // 保存访问节点
        update[i] = x;
    }
    /* we assume the element is not already inside, since we allow duplicated
     * scores, reinserting the same element should never happen since the
     * caller of zslInsert() should test in the hash table if the element is
     * already inside or not. 
     *
     * 我们假设元素不在里面，因为我们允许重复的分数，所以不应该重新插入同一个元素，因为
     * zslInsert（）的调用方应该在哈希表中测试元素是否已经在里面。
     * */
    // 因为这个函数不可能处理两个元素的 member 和 score 都相同的情况，
    // 所以直接创建新节点，不用检查存在性

    // 计算新的随机层数
    level = zslRandomLevel();
    // 如果 level 比当前 skiplist 的最大层数还要大
    // 那么更新 zsl->level 参数
    // 并且初始化 update 和 rank 参数在相应的层的数据
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }
    // 创建新节点
    x = zslCreateNode(level,score,ele);
    // 根据 update 和 rank 两个数组的资料，初始化新节点
    // 并设置相应的指针
    // O(N)
    for (i = 0; i < level; i++) {
        // 设置指针
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;

        /* update span covered by update[i] as x is inserted here 
         *
         * 此处插入x时，更新[i]覆盖的更新跨度
         * */
        // 设置 span
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }

    /* increment span for untouched levels 
     *
     * 增加未受影响级别的跨度
     * */
    // 更新沿途访问节点的 span 值
    for (i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }

    // 设置后退指针
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    // 设置 x 的前进指针
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        // 这个是新的表尾节点
        zsl->tail = x;

    // 更新跳跃表节点数量
    zsl->length++;
    return x;
}

/* Internal function used by zslDelete, zslDeleteRangeByScore and
 * zslDeleteRangeByRank. 
 *
 * zslDelete、zslDeleteRangeByScore和zslDeleteRangeByRank使用的内部函数。
 * */
/*
 * 节点删除函数
 *
 * T = O(N)
 */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    // 修改相应的指针和 span , O(N)
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span -= 1;
        }
    }
    // 处理表头和表尾节点
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    // 收缩 level 的值, O(N)
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL)
        zsl->level--;
    zsl->length--;
}

/* Delete an element with matching score/element from the skiplist.
 * The function returns 1 if the node was found and deleted, otherwise
 * 0 is returned.
 *
 * If 'node' is NULL the deleted node is freed by zslFreeNode(), otherwise
 * it is not freed (but just unlinked) and *node is set to the node pointer,
 * so that it is possible for the caller to reuse the node (including the
 * referenced SDS string at node->ele). 
 *
 * 从skiplist中删除具有匹配分数/元素的元素。如果找到并删除了节点，则函数返
 * 回1，否则返回0。如果“node”为NULL，则删除的节点将由zslFreeNode（）释放，
 * 否则不会释放（只是取消链接），并且将*node设置为节点指针，以便
 * 调用方可以重用该节点（包括node->ele处引用的SDS字符串）。
 * */
/*
 * 从 skiplist 中删除和给定 obj 以及给定 score 匹配的元素
 *
 * T_worst = O(N), T_average = O(log N)
 */
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    x = zsl->header;
    // 遍历所有层，记录删除节点后需要被修改的节点到 update 数组
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
                (x->level[i].forward->score < score ||
                    (x->level[i].forward->score == score &&
                     sdscmp(x->level[i].forward->ele,ele) < 0)))
        {
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. 
     *
     * 我们可能有多个具有相同分数的元素，我们需要的是找到具有正确分数和对象的元素。
     * */
    // 因为多个不同的 member 可能有相同的 score
    // 所以要确保 x 的 member 和 score 都匹配时，才进行删除
    x = x->level[0].forward;
    if (x && score == x->score && sdscmp(x->ele,ele) == 0) {
        zslDeleteNode(zsl, x, update);
        if (!node)
            zslFreeNode(x);
        else
            *node = x;
        return 1;
    }
    return 0; /* not found 
               *
               * 找不到
               * */
}

/* Update the score of an element inside the sorted set skiplist.
 * Note that the element must exist and must match 'score'.
 * This function does not update the score in the hash table side, the
 * caller should take care of it.
 *
 * Note that this function attempts to just update the node, in case after
 * the score update, the node would be exactly at the same position.
 * Otherwise the skiplist is modified by removing and re-adding a new
 * element, which is more costly.
 *
 * The function returns the updated element skiplist node pointer. 
 *
 * 更新排序集skiplist中某个元素的分数。请注意，元素必须存在，并且必须与“s
 * core”匹配。此函数不会更新哈希表中的分数，调用方应该负责。请注意，此函数只尝
 * 试更新节点，以防分数更新后，节点会完全位于同一位置。否则，通过移除并重新添加新元
 * 素来修改skiplist，这会更加昂贵。函数返回更新后的元素skiplist节点
 * 指针。
 * */
zskiplistNode *zslUpdateScore(zskiplist *zsl, double curscore, sds ele, double newscore) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    /* We need to seek to element to update to start: this is useful anyway,
     * we'll have to update or remove it. 
     *
     * 我们需要寻找元素来更新：无论如何，这是有用的，我们必须更新或删除它。
     * */
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
                (x->level[i].forward->score < curscore ||
                    (x->level[i].forward->score == curscore &&
                     sdscmp(x->level[i].forward->ele,ele) < 0)))
        {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Jump to our element: note that this function assumes that the
     * element with the matching score exists. 
     *
     * 跳转到我们的元素：请注意，此函数假设具有匹配分数的元素存在。
     * */
    x = x->level[0].forward;
    serverAssert(x && curscore == x->score && sdscmp(x->ele,ele) == 0);

    /* If the node, after the score update, would be still exactly
     * at the same position, we can just update the score without
     * actually removing and re-inserting the element in the skiplist. 
     *
     * 如果在分数更新后，节点仍然完全在同一位置，我们可以只更新分数，而不必实际删除并重
     * 新插入skiplist中的元素。
     * */
    if ((x->backward == NULL || x->backward->score < newscore) &&
        (x->level[0].forward == NULL || x->level[0].forward->score > newscore))
    {
        x->score = newscore;
        return x;
    }

    /* No way to reuse the old node: we need to remove and insert a new
     * one at a different place. 
     *
     * 没有办法重用旧节点：我们需要在不同的位置移除并插入一个新节点。
     * */
    zslDeleteNode(zsl, x, update);
    zskiplistNode *newnode = zslInsert(zsl,newscore,x->ele);
    /* We reused the old node x->ele SDS string, free the node now
     * since zslInsert created a new one. 
     *
     * 我们重用了旧的节点x->ele SDS字符串，现在释放了这个节点，因为zslInsert创建了一个新的节点。
     * */
    x->ele = NULL;
    zslFreeNode(x);
    return newnode;
}

/*
 * 检查 value 是否属于 spec 指定的范围内
 *
 * T = O(1)
 */
int zslValueGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

/*
 * 检查 value 是否属于 spec 指定的范围内
 *
 * T = O(1)
 */
int zslValueLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Returns if there is a part of the zset is in range. 
 *
 * 如果有一部分zset在范围内，则返回。
 * */
int zslIsInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. 
     *
     * 测试始终为空的范围。
     * */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;
    // 如果 zset 的最大节点的 score 比范围的最小值要小
    // 那么 zset 不在范围之内
    x = zsl->tail;
    if (x == NULL || !zslValueGteMin(x->score,range))
        return 0;
    // 如果 zset 的最小节点的 score 比范围的最大值要大
    // 那么 zset 不在范围之内
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score,range))
        return 0;
    // 在范围内
    return 1;
}

/* Find the first node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找包含在指定范围内的第一个节点。当范围中不包含任何元素时，返回NULL。
 * */
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zslIsInRange(zsl,range)) return NULL;

    x = zsl->header;
    // 找到第一个 score 值大于给定范围最小值的节点
    // O(N)
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *OUT* of range. 
         *
         * *超出范围时继续前进。
         * */
        while (x->level[i].forward &&
            !zslValueGteMin(x->level[i].forward->score,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. 
     *
     * 这是一个内部范围，因此下一个节点不能为NULL。
     * */
    x = x->level[0].forward;
    serverAssert(x != NULL);

    /* Check if score <= max. 
     *
     * 检查分数是否<=最大值。
     * */
    if (!zslValueLteMax(x->score,range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找指定范围中包含的最后一个节点。当范围中不包含任何元素时，返回NULL。
 * */
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zslIsInRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *IN* range. 
         *
         * 在*INrange时继续。
         * */
        while (x->level[i].forward &&
            zslValueLteMax(x->level[i].forward->score,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. 
     *
     * 这是一个内部范围，因此此节点不能为NULL。
     * */
    serverAssert(x != NULL);

    /* Check if score >= min. 
     *
     * 检查分数是否>=最小值。
     * */
    if (!zslValueGteMin(x->score,range)) return NULL;
    return x;
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and max are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
/*
 * 删除给定范围内的 score 的元素。
 *
 * T = O(N^2)
 */
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    // 记录沿途的节点
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (range->minex ?
            x->level[i].forward->score <= range->min :
            x->level[i].forward->score < range->min))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. 
     *
     * 当前节点是最后一个得分<或<=分钟的节点。
     * */
    x = x->level[0].forward;

    /* Delete nodes while in range. 
     *
     * 在范围内删除节点。
     * */
    // 一直向右删除，直到到达 range 的底为止
    // O(N^2)
    while (x &&
           (range->maxex ? x->score < range->max : x->score <= range->max))
    {
        // 保存后继指针
        zskiplistNode *next = x->level[0].forward;
        // 在跳跃表中删除, O(N)
        zslDeleteNode(zsl,x,update);
        // 在字典中删除，O(1)
        dictDelete(dict,x->ele);
        // 释放
        zslFreeNode(x);   /* Here is where x->ele is actually released.
                                 *
                                 * 这是x->ele实际释放的位置。
                                 * */
        removed++;
        x = next;
    }
    return removed;
}

unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;


    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            !zslLexValueGteMin(x->level[i].forward->ele,range))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. 
     *
     * 当前节点是最后一个得分<或<=分钟的节点。
     * */
    x = x->level[0].forward;

    /* Delete nodes while in range. 
     *
     * 在范围内删除节点。
     * */
    while (x && zslLexValueLteMax(x->ele,range)) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->ele);
        zslFreeNode(x);   /* Here is where x->ele is actually released.
                                 *
                                 * 这是x->ele实际释放的位置。
                                 * */
        removed++;
        x = next;
    }
    return removed;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based 
 *
 * 从skiplist中删除所有级别介于开始和结束之间的元素。起点和终点包括在内。请注意，开始和结束需要基于1
 * */
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    // 通过计算 rank ，移动到删除开始的地方
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) < start) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }


    // 算上 start 节点
    traversed++;
    // 从 start 开始，删除直到到达索引 end ，或者末尾
    // O(N^2)
    x = x->level[0].forward;
    while (x && traversed <= end) {
        // 保存后一节点的指针
        zskiplistNode *next = x->level[0].forward;
        // 删除 skiplist 节点, O(N)
        zslDeleteNode(zsl,x,update);
        // 删除 dict 节点, O(1)
        dictDelete(dict,x->ele);
        // 删除节点
        zslFreeNode(x);
        // 删除计数
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. 
 *
 * 根据分数和关键字查找元素的排名。找不到元素时返回0，否则进行排名。请注意，由于zsl->标头到第一个元素的跨度，秩是从1开始的。
 * */
unsigned long zslGetRank(zskiplist *zsl, double score, sds ele) {
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;
    // 遍历 ziplist ，并累积沿途的 span 到 rank ，找到目标元素时返回 rank
    // O(N)
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                sdscmp(x->level[i].forward->ele,ele) <= 0))) {
            // 累积
            rank += x->level[i].span;
            // 前进
            x = x->level[i].forward;
        }

        /* x might be equal to zsl->header, so test if obj is non-NULL 
         *
         * x可能等于zsl->标头，因此测试obj是否为非NULL
         * */
        if (x->ele && sdscmp(x->ele,ele) == 0) {
            // 找到目标元素
            return rank;
        }
    }
    return 0;
}

/* Finds an element by its rank. The rank argument needs to be 1-based. 
 *
 * 按级别查找元素。rank参数需要基于1。
 * */
zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;

    // 沿着指针前进，直到累积的步数 traversed 等于 rank 为止
    // O(N)
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
        {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank) {
            return x;
        }
    }
    // 没找到
    return NULL;
}

/* Populate the rangespec according to the objects min and max. 
 *
 * 根据 min 和 max 对象，将 range 值保存到 spec 上。
 * */
static int zslParseRange(robj *min, robj *max, zrangespec *spec) {
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max 
     *
     * 分析最小-最大间隔。如果其中一个值的前缀是“（”字符，则视为“open”。例如，
     * ZRANGEBYSCORE zset（1.5（2.5）将匹配min<x<max
     * ZRANGEBysCORE zset 1.5 2.5将与min<=x<=max匹配
     * */
    if (min->encoding == OBJ_ENCODING_INT) {
        spec->min = (long)min->ptr;
    } else {
        if (((char*)min->ptr)[0] == '(') {
            spec->min = strtod((char*)min->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
            spec->minex = 1;
        } else {
            spec->min = strtod((char*)min->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
        }
    }
    if (max->encoding == OBJ_ENCODING_INT) {
        spec->max = (long)max->ptr;
    } else {
        if (((char*)max->ptr)[0] == '(') {
            spec->max = strtod((char*)max->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
            spec->maxex = 1;
        } else {
            spec->max = strtod((char*)max->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
        }
    }

    return C_OK;
}

/* ------------------------ Lexicographic ranges ---------------------------- 
                            词典范围
*/

/* Parse max or min argument of ZRANGEBYLEX.
  * (foo means foo (open interval)
  * [foo means foo (closed interval)
  * - means the min string possible
  * + means the max string possible
  *
  * If the string is valid the *dest pointer is set to the redis object
  * that will be used for the comparison, and ex will be set to 0 or 1
  * respectively if the item is exclusive or inclusive. C_OK will be
  * returned.
  *
  * If the string is not a valid range C_ERR is returned, and the value
  * of *dest and *ex is undefined. 
 *
 * 分析ZRANGEBYLEX的最大或最小参数。（foo表示foo（开放间隔）-表示
 * 可能的最小字符串+表示可能的最大字符串如果字符串有效，则将*dest指针设置为将
 * 用于比较的redis对象，如果项为独占或包含项，则将ex分别设置为0或1。将返回
 * C_OK。如果字符串不是有效范围，则返回C_ERR，并且*dest和*ex的值未
 * 定义。
 * */
int zslParseLexRangeItem(robj *item, sds *dest, int *ex) {
    char *c = item->ptr;

    switch(c[0]) {
    case '+':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.maxstring;
        return C_OK;
    case '-':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.minstring;
        return C_OK;
    case '(':
        *ex = 1;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    case '[':
        *ex = 0;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    default:
        return C_ERR;
    }
}

/* Free a lex range structure, must be called only after zelParseLexRange()
 * populated the structure with success (C_OK returned). 
 *
 * 释放lex范围结构，必须在zelParseLexRange（）成功填充该结构后才
 * 能调用（返回C_OK）。
 * */
void zslFreeLexRange(zlexrangespec *spec) {
    if (spec->min != shared.minstring &&
        spec->min != shared.maxstring) sdsfree(spec->min);
    if (spec->max != shared.minstring &&
        spec->max != shared.maxstring) sdsfree(spec->max);
}

/* Populate the lex rangespec according to the objects min and max.
 *
 * Return C_OK on success. On error C_ERR is returned.
 * When OK is returned the structure must be freed with zslFreeLexRange(),
 * otherwise no release is needed. 
 *
 * 根据对象最小值和最大值填充lex rangespec。成功时返回C_OK。返回错
 * 误时C_ERR。当返回OK时，必须使用zslFreeLexRange（）释放结构，
 * 否则不需要释放。
 * */
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec) {
    /* The range can't be valid if objects are integer encoded.
     * Every item must start with ( or [. 
     *
     * 如果对象是整数编码的，则该范围无效。每个项目都必须以（或[。
     * */
    if (min->encoding == OBJ_ENCODING_INT ||
        max->encoding == OBJ_ENCODING_INT) return C_ERR;

    spec->min = spec->max = NULL;
    if (zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR ||
        zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR) {
        zslFreeLexRange(spec);
        return C_ERR;
    } else {
        return C_OK;
    }
}

/* This is just a wrapper to sdscmp() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings 
 *
 * 这只是sdscmp（）的一个包装器，它能够处理shared.minstring和
 * shared.maxstring，相当于字符串的-inf和+inf
 * */
int sdscmplex(sds a, sds b) {
    if (a == b) return 0;
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    return sdscmp(a,b);
}

int zslLexValueGteMin(sds value, zlexrangespec *spec) {
    return spec->minex ?
        (sdscmplex(value,spec->min) > 0) :
        (sdscmplex(value,spec->min) >= 0);
}

int zslLexValueLteMax(sds value, zlexrangespec *spec) {
    return spec->maxex ?
        (sdscmplex(value,spec->max) < 0) :
        (sdscmplex(value,spec->max) <= 0);
}

/* Returns if there is a part of the zset is in the lex range. 
 *
 * 如果zset的一部分在lex范围内，则返回。
 * */
int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. 
     *
     * 测试始终为空的范围。
     * */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    if (x == NULL || !zslLexValueGteMin(x->ele,range))
        return 0;
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslLexValueLteMax(x->ele,range))
        return 0;
    return 1;
}

/* Find the first node that is contained in the specified lex range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找包含在指定lex范围中的第一个节点。当范围中不包含任何元素时，返回NULL。
 * */
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *OUT* of range. 
         *
         * *超出范围时继续前进。
         * */
        while (x->level[i].forward &&
            !zslLexValueGteMin(x->level[i].forward->ele,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. 
     *
     * 这是一个内部范围，因此下一个节点不能为NULL。
     * */
    x = x->level[0].forward;
    serverAssert(x != NULL);

    /* Check if score <= max. 
     *
     * 检查分数是否<=最大值。
     * */
    if (!zslLexValueLteMax(x->ele,range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找指定范围中包含的最后一个节点。当范围中不包含任何元素时，返回NULL。
 * */
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *IN* range. 
         *
         * 在*INrange时继续。
         * */
        while (x->level[i].forward &&
            zslLexValueLteMax(x->level[i].forward->ele,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. 
     *
     * 这是一个内部范围，因此此节点不能为NULL。
     * */
    serverAssert(x != NULL);

    /* Check if score >= min. 
     *
     * 检查分数是否>=最小值。
     * */
    if (!zslLexValueGteMin(x->ele,range)) return NULL;
    return x;
}

/*-----------------------------------------------------------------------------
 * Ziplist-backed sorted set API
 * Ziplist-based排序集API
 *----------------------------------------------------------------------------*/

/*
 * 取出 sptr 所指向的 ziplist 节点的 score 值
 *
 * T = O(1)
 */
double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    char buf[128];
    double score;

    serverAssert(sptr != NULL);
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr) {
        // 字符串值
        // 用在这里表示 score 是一个非常大的整数
        // （超过 long long 类型）
        // 或者一个浮点数
        memcpy(buf,vstr,vlen);
        buf[vlen] = '\0';
        score = strtod(buf,NULL);
    } else {
        // 整数值
        score = vlong;
    }

    return score;
}

/* Return a ziplist element as an SDS string. 
 *
 * 将ziplist元素作为SDS字符串返回。
 * */
sds ziplistGetObject(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    serverAssert(sptr != NULL);
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr) {
        return sdsnewlen((char*)vstr,vlen);
    } else {
        return sdsfromlonglong(vlong);
    }
}

/* Compare element in sorted set with given element. 
 *
 * 将排序集中的元素与给定元素进行比较。
 * */
int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    serverAssert(ziplistGet(eptr,&vstr,&vlen,&vlong));
    if (vstr == NULL) {
        /* Store string representation of long long in buf. 
         *
         * 将long-long的字符串表示形式存储在buf中。
         * */
        // 如果节点保存的是整数值，
        // 那么将它转换为字符串表示
        vlen = ll2string((char*)vbuf,sizeof(vbuf),vlong);
        vstr = vbuf;
    }

    // 对比
    // 小优化（只对比长度较短的字符串的长度）
    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr,cstr,minlen);
    if (cmp == 0) return vlen-clen;
    return cmp;
}

/*
 * 返回 ziplist 表示的有序集的长度
 *
 * T = O(N)
 */
unsigned int zzlLength(unsigned char *zl) {
    // 每个有序集用两个 ziplist 节点表示
    // O(N)
    return ziplistLen(zl)/2;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. 
 *
 * 根据eptr和sptr中的值移动到下一个节点。当没有下一个节点时，两者都设置为NULL。
 * */
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    // 指向下一节点的 member 域
    _eptr = ziplistNext(zl,*sptr);
    if (_eptr != NULL) {
        // 指向下一节点的 score 域
        _sptr = ziplistNext(zl,_eptr);
        serverAssert(_sptr != NULL);
    } else {
        /* No next entry. 
         *
         * 没有下一个节点。
         * */
        _sptr = NULL;
    }

    // 更新指针
    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no next entry. 
 *
 * 根据eptr和sptr中的值移动到上一个节点。当没有下一个节点时，两者都设置为NULL。
 * */
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    // 指向前一节点的 score 域
    _sptr = ziplistPrev(zl,*eptr);
    if (_sptr != NULL) {
        // 指向前一节点的 memeber 域
        _eptr = ziplistPrev(zl,_sptr);
        serverAssert(_eptr != NULL);
    } else {
        /* No previous entry. 
         *
         * 没有以前的节点。
         * */
        _eptr = NULL;
    }

    // 更新指针
    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. 
 *
 * 如果有一部分zset在范围内，则返回。应仅由zzlFirstInRange和zzlLastInRange内部使用。
 * */
/*
 * 检查有序集的 score 值是否在给定的 range 之内
 *
 * T = O(1)
 */
int zzlIsInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. 
     *
     * 测试始终为空的范围。
     * */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    // 取出有序集中最小的 score 值
    p = ziplistIndex(zl,-1);   /* Last score.
                                      *
                                      * 上次得分。
                                      * */
    if (p == NULL) return 0; /* Empty sorted set 
                              *
                              * 空排序集
                              * */
    score = zzlGetScore(p);
    // 如果 score 值不位于给定边界之内，返回 0
    if (!zslValueGteMin(score,range))
        return 0;

    // 取出有序集中最大的 score 值
    p = ziplistIndex(zl,1);   /* First score.
                                     *
                                     * 第一个得分。
                                     * */
    serverAssert(p != NULL);
    score = zzlGetScore(p);
    // 如果 score 值不位于给定边界之内，返回 0
    if (!zslValueLteMax(score,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. 
 *
 * 指向指定范围中包含的第一个元素的查找指针。当范围中不包含任何元素时，返回NULL
 * 。
 * */
/*
 * 返回第一个 score 值在给定范围内的节点
 *
 * 如果没有节点的 score 值在给定范围，返回 NULL 。
 *
 * T = O(N)
 */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range) {
    // 从表头开始遍历
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double score;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zzlIsInRange(zl,range)) return NULL;

    // 从表头向表尾遍历
    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        // 获取 score 值
        score = zzlGetScore(sptr);
        // score 值在范围之内？
        if (zslValueGteMin(score,range)) {
            /* Check if score <= max. 
             *
             * 检查分数是否<=最大值。
             * */
            if (zslValueLteMax(score,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. 
         *
         * 移动到下一个元素。
         * */
        eptr = ziplistNext(zl,sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找指向指定范围中包含的最后一个元素的指针。当范围中不包含任何元素时，返回NULL。
 * */
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range) {
    // 从表尾开始遍历
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;
    double score;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zzlIsInRange(zl,range)) return NULL;

    // 在有序的 ziplist 里从表尾到表头遍历
    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        // 获取节点的 score 值
        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Check if score >= min. 
             *
             * 检查分数是否>=最小值。
             * */
            // score 在给定的范围之内？
            if (zslValueGteMin(score,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. 
         *
         * 通过移动到上一个元素的分数来移动到上个元素。当它返回NULL时，我们知道也没有元素。
         * */
        // 前移指针
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec) {
    sds value = ziplistGetObject(p);
    int res = zslLexValueGteMin(value,spec);
    sdsfree(value);
    return res;
}

int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec) {
    sds value = ziplistGetObject(p);
    int res = zslLexValueLteMax(value,spec);
    sdsfree(value);
    return res;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. 
 *
 * 如果有一部分zset在范围内，则返回。应仅由zzlFirstInRange和zz
 * lLastInRange内部使用。
 * */
int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *p;

    /* Test for ranges that will always be empty. 
     *
     * 测试始终为空的范围。
     * */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;

    p = ziplistIndex(zl,-2); /* Last element. 
                              *
                              * 最后一个元素。
                              * */
    if (p == NULL) return 0;
    if (!zzlLexValueGteMin(p,range))
        return 0;

    p = ziplistIndex(zl,0); /* First element. 
                             *
                             * 第一个元素。
                             * */
    serverAssert(p != NULL);
    if (!zzlLexValueLteMax(p,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找指向指定lex范围中包含的第一个元素的指针。当范围中不包含任何元素时，返回NULL。
 * */
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueGteMin(eptr,range)) {
            /* Check if score <= max. 
             *
             * 检查分数是否<=最大值。
             * */
            if (zzlLexValueLteMax(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. 
         *
         * 移动到下一个元素。
         * */
        sptr = ziplistNext(zl,eptr); /* This element score. Skip it. 
                                      *
                                      * 此元素得分。跳过它。
                                      * */
        serverAssert(sptr != NULL);
        eptr = ziplistNext(zl,sptr); /* Next element. 
                                      *
                                      * 下一个元素。
                                      * */
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. 
 *
 * 查找指向指定lex范围中包含的最后一个元素的指针。当范围中不包含任何元素时，返回NULL。
 * */
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;

    /* If everything is out of range, return early. 
     *
     * 如果一切都超出范围，请提前返回。
     * */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Check if score >= min. 
             *
             * 检查分数是否>=最小值。
             * */
            if (zzlLexValueGteMin(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. 
         *
         * 通过移动到上一个元素的分数来移动到上个元素。当它返回NULL时，我们知道也没有元
         * 素。
         * */
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

/*
 * 在 ziplist 里查找给定元素 ele ，如果找到了，
 * 将元素的点数保存到 score ，并返回该元素在 ziplist 的指针。
 *
 * T = O(N^2)
 */
unsigned char *zzlFind(unsigned char *zl, sds ele, double *score) {
    // 迭代器
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        // 对比元素 ele 的值和 eptr 所保存的值
        // O(N)
        if (ziplistCompare(eptr,(unsigned char*)ele,sdslen(ele))) {
            /* Matching element, pull out score. 
             *
             * 将匹配元素的指针保存到 score 里
             * */
            if (score != NULL) *score = zzlGetScore(sptr);
            return eptr;
        }

        /* Move to next element. 
         *
         * 移动到下一个元素。
         * */
        // 返回指针
        eptr = ziplistNext(zl,sptr);
    }
    return NULL;
}

/* Delete (element,score) pair from ziplist. Use local copy of eptr because we
 * don't want to modify the one given as argument. 
 *
 * 从拉链列表中删除（元素，分数）对。使用eptr的本地副本，因为我们不想修改作为参
 * 数给定的副本。
 * */
/*
 * 从 ziplist 中删除 element-score 对。
 * 使用一个副本保存 eptr 的值。
 *
 * T = O(N^2)
 */
unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr) {
    unsigned char *p = eptr;

    /* TODO: add function to ziplist API to delete N elements from offset. 
     *
     * TODO:将函数添加到ziplist API以从偏移量中删除N个元素。
     * */
    // 删除 member 域 ，O(N^2)
    zl = ziplistDelete(zl,&p);
    // 删除 score 域 ，O(N^2)
    zl = ziplistDelete(zl,&p);
    return zl;
}

/*
 * 将有序集节点保存到 eptr 所指向的地方
 *
 * T = O(N^2)
 */
unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, sds ele, double score) {
    unsigned char *sptr;
    char scorebuf[128];
    int scorelen;
    size_t offset;

    // 将 score 值转换为字符串
    scorelen = d2string(scorebuf,sizeof(scorebuf),score);
    if (eptr == NULL) {
        // 插入到 ziplist 的最后, O(N^2)
        // ziplist 的第一个节点保存有序集的 member
        zl = ziplistPush(zl,(unsigned char*)ele,sdslen(ele),ZIPLIST_TAIL);
        // ziplist 的第二个节点保存有序集的 score
        zl = ziplistPush(zl,(unsigned char*)scorebuf,scorelen,ZIPLIST_TAIL);
    } else {
        // 插入到给定位置, O(N^2)
        /* Keep offset relative to zl, as it might be re-allocated. 
         *
         * 保持相对于zl的偏移量，因为它可能会被重新分配。
         * */
        offset = eptr-zl;
        // 保存 member
        zl = ziplistInsert(zl,eptr,(unsigned char*)ele,sdslen(ele));
        eptr = zl+offset;

        /* Insert score after the element. 
         *
         * 在元素后面插入分数。
         * */
        serverAssert((sptr = ziplistNext(zl,eptr)) != NULL);
        // 保存 score
        zl = ziplistInsert(zl,sptr,(unsigned char*)scorebuf,scorelen);
    }
    return zl;
}

/* Insert (element,score) pair in ziplist. This function assumes the element is
 * not yet present in the list. 
 *
 * 在拉链中插入（元素，得分）一双。此函数假定该元素尚未出现在列表中。
 * */
/*
 * 将 ele 成员和它的分值 score 添加到 ziplist 里面
 *
 * ziplist 里的各个节点按 score 值从小到大排列
 *
 * 这个函数假设 elem 不存在于有序集
 *
 * T = O(N^2)
 */
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score) {
    // 指向 ziplist 第一个节点（也即是有序集的 member 域）
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double s;

    // 遍历整个 ziplist
    while (eptr != NULL) {
        // 指向 score 域
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);
        // 取出 score 值
        s = zzlGetScore(sptr);

        if (s > score) {
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. 
             *
             * 得分大于要插入元素得分的第一个元素。这意味着我们应该在列表中占据它的位置，以保持
             * 秩序。
             * */
            // 遇到第一个 score 值比输入 score 大的节点
            // 将新节点插入在这个节点的前面，
            // 让节点在 ziplist 里根据 score 从小到大排列
            // O(N^2)
            zl = zzlInsertAt(zl,eptr,ele,score);
            break;
        } else if (s == score) {
            /* Ensure lexicographical ordering for elements. 
             *
             * 确保元素按字典顺序排列。
             * */
            // 如果输入 score 和节点的 score 相同
            // 那么根据 member 的字符串位置来决定新节点的插入位置
            if (zzlCompareElements(eptr,(unsigned char*)ele,sdslen(ele)) > 0) {
                zl = zzlInsertAt(zl,eptr,ele,score);
                break;
            }
        }

        /* Move to next element. 
         *
         * 移动到下一个元素。
         * */
        eptr = ziplistNext(zl,sptr);
    }

    /* Push on tail of list when it was not yet inserted. 
     *
     * 当列表的尾部还没有插入时，按下它。
     * */
    // 如果有序集里目前没有一个节点的 score 值比输入 score 大
    // 那么将新节点添加到 ziplist 的最后
    if (eptr == NULL)
        zl = zzlInsertAt(zl,NULL,ele,score);
    return zl;
}

/*
 * 删除给定 score 范围内的节点
 *
 * T = O(N^3)
 */
unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    // 按 range 定位起始节点的位置
    eptr = zzlFirstInRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. 
     *
     * 当ziplist的尾部被删除时，eptr将指向sentinel字节，ziplistNext将返回NULL。
     * */
    // 一直进行删除，直到碰到 score 值比 range->max 更大的节点为止
    // O(N^3)
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Delete both the element and the score. 
             *
             * 删除元素和分数。
             * */
            // O(N^2)
            zl = ziplistDelete(zl,&eptr);
            // O(N^2)
            zl = ziplistDelete(zl,&eptr);
            num++;
        } else {
            /* No longer in range. 
             *
             * 不再在射程内。
             * */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

unsigned char *zzlDeleteRangeByLex(unsigned char *zl, zlexrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInLexRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. 
     *
     * 当ziplist的尾部被删除时，eptr将指向sentinel字节，ziplistNext将返回NULL。
     * */
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Delete both the element and the score. 
             *
             * 删除元素和分数。
             * */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            num++;
        } else {
            /* No longer in range. 
             *
             * 不再在射程内。
             * */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based 
 *
 * 从skiplist中删除所有级别介于开始和结束之间的元素。起点和终点包括在内。请注意，开始和结束需要基于1
 * */
unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted) {
    // 计算删除的节点数量
    unsigned int num = (end-start)+1;
    if (deleted) *deleted = num;
    // 删除
    zl = ziplistDeleteRange(zl,2*(start-1),2*num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 *---------------------------------------------------------------------------- */

/*
 * 返回有序集的元素个数
 *
 * T = O(N)
 */
unsigned long zsetLength(const robj *zobj) {
    unsigned long length = 0;
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        length = zzlLength(zobj->ptr);
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        length = ((const zset*)zobj->ptr)->zsl->length;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return length;
}

/*
 * 将给定的 zobj 转换成给定编码
 *
 * T = O(N^3)
 */
void zsetConvert(robj *zobj, int encoding) {
    zset *zs;
    zskiplistNode *node, *next;
    sds ele;
    double score;

    // 编码相同，无须转换
    if (zobj->encoding == encoding) return;

    // 将 ziplist 编码转换成 skiplist 编码
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != OBJ_ENCODING_SKIPLIST)
            serverPanic("Unknown target encoding");

        // 创建新 zset
        zs = zmalloc(sizeof(*zs));
        zs->dict = dictCreate(&zsetDictType,NULL);
        zs->zsl = zslCreate();

        // 指向第一个节点的 member 域
        eptr = ziplistIndex(zl,0);
        serverAssertWithInfo(NULL,zobj,eptr != NULL);
        // 指向第一个节点的 score 域
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(NULL,zobj,sptr != NULL);

        // 遍历整个 ziplist ，将它的 member 和 score 添加到 zset
        // O(N^2)
        while (eptr != NULL) {
            // 取出 score 值
            score = zzlGetScore(sptr);
            // 取出 member 值
            serverAssertWithInfo(NULL,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));
            // 为 member 值创建 robj 对象
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen((char*)vstr,vlen);

            // 将 score 和 member（这里的ele）添加到 skiplist
            // O(N)
            node = zslInsert(zs->zsl,score,ele);
            // 将 member 作为键， score 作为值，保存到字典
            // O(1)
            serverAssert(dictAdd(zs->dict,ele,&node->score) == DICT_OK);
            // 前进至下个节点
            zzlNext(zl,&eptr,&sptr);
        }

        zfree(zobj->ptr);
        zobj->ptr = zs;
        zobj->encoding = OBJ_ENCODING_SKIPLIST;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        // 将 skiplist 转换为 ziplist


        unsigned char *zl = ziplistNew();

        if (encoding != OBJ_ENCODING_ZIPLIST)
            serverPanic("Unknown target encoding");

        /* Approach similar to zslFree(), since we want to free the skiplist at
         * the same time as creating the ziplist. 
         *
         * 方法类似于zslFree（），因为我们希望在创建ziplist的同时释放skiplist。
         * */
        zs = zobj->ptr;
        // 释放整个字典
        dictRelease(zs->dict);
        // 指向首个节点
        node = zs->zsl->header->level[0].forward;
        // 释放 zset 表头
        zfree(zs->zsl->header);
        zfree(zs->zsl);

        // 将所有元素保存到 ziplist , O(N^3)
        while (node) {
            // 插入 member 和 score 到 ziplist, O(N^2)
            zl = zzlInsertAt(zl,NULL,node->ele,node->score);
            next = node->level[0].forward;
            zslFreeNode(node);
            node = next;
        }

        zfree(zs);
        zobj->ptr = zl;
        zobj->encoding = OBJ_ENCODING_ZIPLIST;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* Convert the sorted set object into a ziplist if it is not already a ziplist
 * and if the number of elements and the maximum element size and total elements size
 * are within the expected ranges. 
 *
 * 如果已排序的集合对象还不是ziplist，并且元素数量、最大元素大小和总元素大小
 * 在预期范围内，则将其转换为ziplist。
 * */
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen) {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) return;
    zset *zset = zobj->ptr;

    if (zset->zsl->length <= server.zset_max_ziplist_entries &&
        maxelelen <= server.zset_max_ziplist_value &&
        ziplistSafeToAdd(NULL, totelelen))
    {
        zsetConvert(zobj,OBJ_ENCODING_ZIPLIST);
    }
}

/* Return (by reference) the score of the specified member of the sorted set
 * storing it into *score. If the element does not exist C_ERR is returned
 * otherwise C_OK is returned and *score is correctly populated.
 * If 'zobj' or 'member' is NULL, C_ERR is returned. 
 *
 * 返回（通过引用）已排序集合中指定成员的分数，将其存储到*score中。如果元素不
 * 存在，则返回C_ERR，否则返回C_OK并正确填充*score。如果“zobj”
 * 或“member”为NULL，则返回C_ERR。
 * */
int zsetScore(robj *zobj, sds member, double *score) {
    if (!zobj || !member) return C_ERR;

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        if (zzlFind(zobj->ptr, member, score) == NULL) return C_ERR;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de = dictFind(zs->dict, member);
        if (de == NULL) return C_ERR;
        *score = *(double*)dictGetVal(de);
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return C_OK;
}

/* Add a new element or update the score of an existing element in a sorted
 * set, regardless of its encoding.
 *
 * The set of flags change the command behavior. They are passed with an integer
 * pointer since the function will clear the flags and populate them with
 * other flags to indicate different conditions.
 *
 * The input flags are the following:
 *
 * ZADD_INCR: Increment the current element score by 'score' instead of updating
 *            the current element score. If the element does not exist, we
 *            assume 0 as previous score.
 * ZADD_NX:   Perform the operation only if the element does not exist.
 * ZADD_XX:   Perform the operation only if the element already exist.
 *
 * When ZADD_INCR is used, the new score of the element is stored in
 * '*newscore' if 'newscore' is not NULL.
 *
 * The returned flags are the following:
 *
 * ZADD_NAN:     The resulting score is not a number.
 * ZADD_ADDED:   The element was added (not present before the call).
 * ZADD_UPDATED: The element score was updated.
 * ZADD_NOP:     No operation was performed because of NX or XX.
 *
 * Return value:
 *
 * The function returns 1 on success, and sets the appropriate flags
 * ADDED or UPDATED to signal what happened during the operation (note that
 * none could be set if we re-added an element using the same score it used
 * to have, or in the case a zero increment is used).
 *
 * The function returns 0 on error, currently only when the increment
 * produces a NAN condition, or when the 'score' value is NAN since the
 * start.
 *
 * The command as a side effect of adding a new element may convert the sorted
 * set internal encoding from ziplist to hashtable+skiplist.
 *
 * Memory management of 'ele':
 *
 * The function does not take ownership of the 'ele' SDS string, but copies
 * it if needed. 
 *
 * 添加新元素或更新排序集中现有元素的分数，无论其编码如何。标志集可更改命令行为。它
 * 们是用一个整数指针传递的，因为函数将清除标志并用其他标志填充它们以指示不同的条件
 * 。输入标志如下：
 *
 * ZADD_INCR：通过“score”增加当前元素分数，而不是更新当前元素分数。如果元素不存在，我们假设0为上一个分数。
 * ZADD_NX：只有当元素不存在时才执行操作。
 * ZADD_XX：只有当元素已经存在时才执行该操作。使用ZADD_INCR时，如果“newscore”不为NULL，则元素的新分数将存储在“*newscore“中。返回的标志如下：ZADD_NAME:得到的分数不是数字。
 * ZADD_ADDED:元素已添加（在调用之前不存在）。
 * ZADD_UPDATED:元素分数已更新。
 * ZADD_NOP：由于NX或XX，未执行任何操作。
 *
 * 返回值：
 *
 * 函数在成功时返回1，并设置适当的标志ADDED或UPDATED以表示操作过程中发生的情
 * 况（注意，如果我们使用与以前相同的分数重新添加元素，或者在使用零增量的情况下，则
 * 不能设置任何标志）。该函数在出现错误时返回0，当前仅当增量产生NAN条件时，或者
 * 当“分数”值自开始以来为NAN时。该命令作为添加新元素的副作用，可以将已排序的集
 * 合内部编码从ziplist转换为hashtable+skiplist。“ele”
 * 的内存管理：
 *
 * 该函数不拥有“ele“SDS字符串的所有权，但会在需要时复制它。
 * */
int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore) {
    /* Turn options into simple to check vars. 
     *
     * 将选项转换为易于检查的变量。
     * */
    int incr = (*flags & ZADD_INCR) != 0;
    int nx = (*flags & ZADD_NX) != 0;
    int xx = (*flags & ZADD_XX) != 0;
    *flags = 0; /* We'll return our response flags. 
                 *
                 * 我们将返回我们的响应标志。
                 * */
    double curscore;

    /* NaN as input is an error regardless of all the other parameters. 
     *
     * 作为输入的NaN是一个错误，与所有其他参数无关。
     * */
    if (isnan(score)) {
        *flags = ZADD_NAN;
        return 0;
    }

    /* Update the sorted set according to its encoding. 
     *
     * 根据编码更新已排序的集合。
     * */
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL) {
            /* NX? Return, same element already exists. 
             *
             * NX？返回，已存在相同的元素。
             * */
            if (nx) {
                *flags |= ZADD_NOP;
                return 1;
            }

            /* Prepare the score for the increment if needed. 
             *
             * 如果需要，为增量准备分数。
             * */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *flags |= ZADD_NAN;
                    return 0;
                }
                if (newscore) *newscore = score;
            }

            /* Remove and re-insert when score changed. 
             *
             * 刻痕发生变化时，取出并重新插入。
             * */
            if (score != curscore) {
                zobj->ptr = zzlDelete(zobj->ptr,eptr);
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                *flags |= ZADD_UPDATED;
            }
            return 1;
        } else if (!xx) {
            /* check if the element is too large or the list
             * becomes too long *before* executing zzlInsert. 
             *
             * 在执行zzlInsert之前，请检查元素是否过大或列表是否过长*。
             * */
            if (zzlLength(zobj->ptr)+1 > server.zset_max_ziplist_entries ||
                sdslen(ele) > server.zset_max_ziplist_value ||
                !ziplistSafeToAdd(zobj->ptr, sdslen(ele)))
            {
                zsetConvert(zobj,OBJ_ENCODING_SKIPLIST);
            } else {
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                if (newscore) *newscore = score;
                *flags |= ZADD_ADDED;
                return 1;
            }
        } else {
            *flags |= ZADD_NOP;
            return 1;
        }
    }

    /* Note that the above block handling ziplist would have either returned or
     * converted the key to skiplist. 
     *
     * 请注意，上面的块处理ziplist会返回或将键转换为skiplist。
     * */
    if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplistNode *znode;
        dictEntry *de;

        de = dictFind(zs->dict,ele);
        if (de != NULL) {
            /* NX? Return, same element already exists. 
             *
             * NX？返回，已存在相同的元素。
             * */
            if (nx) {
                *flags |= ZADD_NOP;
                return 1;
            }
            curscore = *(double*)dictGetVal(de);

            /* Prepare the score for the increment if needed. 
             *
             * 如果需要，为增量准备分数。
             * */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *flags |= ZADD_NAN;
                    return 0;
                }
                if (newscore) *newscore = score;
            }

            /* Remove and re-insert when score changes. 
             *
             * 分数发生变化时移除并重新插入。
             * */
            if (score != curscore) {
                znode = zslUpdateScore(zs->zsl,curscore,ele,score);
                /* Note that we did not removed the original element from
                 * the hash table representing the sorted set, so we just
                 * update the score. 
                 *
                 * 请注意，我们没有从表示排序集的哈希表中删除原始元素，所以我们只是更新分数。
                 * */
                dictGetVal(de) = &znode->score; /* Update score ptr. 
                                                 *
                                                 * 更新分数ptr。
                                                 * */
                *flags |= ZADD_UPDATED;
            }
            return 1;
        } else if (!xx) {
            ele = sdsdup(ele);
            znode = zslInsert(zs->zsl,score,ele);
            serverAssert(dictAdd(zs->dict,ele,&znode->score) == DICT_OK);
            *flags |= ZADD_ADDED;
            if (newscore) *newscore = score;
            return 1;
        } else {
            *flags |= ZADD_NOP;
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* Never reached. 
               *
               * 从未联系过。
               * */
}

/* Delete the element 'ele' from the sorted set, returning 1 if the element
 * existed and was deleted, 0 otherwise (the element was not there). 
 *
 * 从排序集中删除元素“ele”，如果该元素存在并已删除，则返回1，否则返回0（该元素不在）。
 * */
int zsetDel(robj *zobj, sds ele) {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,NULL)) != NULL) {
            zobj->ptr = zzlDelete(zobj->ptr,eptr);
            return 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de;
        double score;

        de = dictUnlink(zs->dict,ele);
        if (de != NULL) {
            /* Get the score in order to delete from the skiplist later. 
             *
             * 获取分数以便稍后从skiplist中删除。
             * */
            score = *(double*)dictGetVal(de);

            /* Delete from the hash table and later from the skiplist.
             * Note that the order is important: deleting from the skiplist
             * actually releases the SDS string representing the element,
             * which is shared between the skiplist and the hash table, so
             * we need to delete from the skiplist as the final step. 
             *
             * 从哈希表中删除，稍后从skiplist中删除。请注意，顺序很重要：从skiplist中删除实际上会释放表示元素的SDS字符串，
             * 该字符串在skiplist和哈希表之间共享，因此我们需要从skiplist中删除作为最后一步。
             * */
            dictFreeUnlinkedEntry(zs->dict,de);

            /* Delete from skiplist. 
             *
             * 从skiplist中删除。
             * */
            int retval = zslDelete(zs->zsl,score,ele,NULL);
            serverAssert(retval);

            if (htNeedsResize(zs->dict)) dictResize(zs->dict);
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* No such element found. 
               *
               * 找不到这样的元素。
               * */
}

/* Given a sorted set object returns the 0-based rank of the object or
 * -1 if the object does not exist.
 *
 * For rank we mean the position of the element in the sorted collection
 * of elements. So the first element has rank 0, the second rank 1, and so
 * forth up to length-1 elements.
 *
 * If 'reverse' is false, the rank is returned considering as first element
 * the one with the lowest score. Otherwise if 'reverse' is non-zero
 * the rank is computed considering as element with rank 0 the one with
 * the highest score. 
 *
 * 给定一个排序的集合对象，返回该对象基于0的秩，如果该对象不存在，则返回-1。
 *
 * 对于秩，我们指的是元素在已排序的元素集合中的位置。因此，第一个元素具有秩0，第二个秩
 * 1，依此类推，直到长度为1的元素。
 *
 * 如果“reverse”为false，则返回排名，将得分最低的元素视为第一个元素。
 * 否则，如果“reverse”为非零，则将具有最高分数的元素视为秩为0的元素来计算秩。
 * */
long zsetRank(robj *zobj, sds ele, int reverse) {
    unsigned long llen;
    unsigned long rank;

    llen = zsetLength(zobj);

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = ziplistIndex(zl,0);
        serverAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        rank = 1;
        while(eptr != NULL) {
            if (ziplistCompare(eptr,(unsigned char*)ele,sdslen(ele)))
                break;
            rank++;
            zzlNext(zl,&eptr,&sptr);
        }

        if (eptr != NULL) {
            if (reverse)
                return llen-rank;
            else
                return rank-1;
        } else {
            return -1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;
        double score;

        de = dictFind(zs->dict,ele);
        if (de != NULL) {
            score = *(double*)dictGetVal(de);
            rank = zslGetRank(zsl,score,ele);
            /* Existing elements always have a rank. 
             *
             * 现有元素总是有等级的。
             * */
            serverAssert(rank != 0);
            if (reverse)
                return llen-rank;
            else
                return rank-1;
        } else {
            return -1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Sorted set commands
 * 排序的集合命令
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY. 
 *
 * 此通用命令实现ZADD和ZINCRBY。
 * */
/*
 * 多态添加操作
 *
 * ZADD 和 ZINCRBY 的底层实现
 *
 * T = O(N^4)
 */
void zaddGenericCommand(client *c, int flags) {
    static char *nanerr = "resulting score is not a number (NaN)";
    robj *key = c->argv[1];
    robj *zobj;
    sds ele;
    double score = 0, *scores = NULL;
    int j, elements;
    int scoreidx = 0;
    /* The following vars are used in order to track what the command actually
     * did during the execution, to reply to the client and to trigger the
     * notification of keyspace change. 
     *
     * 以下变量用于跟踪命令在执行过程中的实际操作，回复客户端并触发键空间更改的通知。
     * */
    int added = 0;      /* Number of new elements added. 
                         *
                         * 添加的新元素数量。
                         * */
    int updated = 0;    /* Number of elements with updated score. 
                         *
                         * 分数更新的元素数。
                         * */
    int processed = 0;  /* Number of elements processed, may remain zero with
                           options like XX. 
                         *
                         * 处理的元素数量，可以保持零，选项如XX。
                         * */

    /* Parse options. At the end 'scoreidx' is set to the argument position
     * of the score of the first score-element pair. 
     *
     * 分析选项。在末尾，“scoreidx”被设置为第一个score元素对的score
     * 的参数位置。
     * */
    scoreidx = 2;
    while(scoreidx < c->argc) {
        char *opt = c->argv[scoreidx]->ptr;
        if (!strcasecmp(opt,"nx")) flags |= ZADD_NX;
        else if (!strcasecmp(opt,"xx")) flags |= ZADD_XX;
        else if (!strcasecmp(opt,"ch")) flags |= ZADD_CH;
        else if (!strcasecmp(opt,"incr")) flags |= ZADD_INCR;
        else break;
        scoreidx++;
    }

    /* Turn options into simple to check vars. 
     *
     * 将选项转换为易于检查的变量。
     * */
    int incr = (flags & ZADD_INCR) != 0;
    int nx = (flags & ZADD_NX) != 0;
    int xx = (flags & ZADD_XX) != 0;
    int ch = (flags & ZADD_CH) != 0;

    /* After the options, we expect to have an even number of args, since
     * we expect any number of score-element pairs. 
     *
     * 在选项之后，我们希望有偶数个args，因为我们希望有任意数量的score元素对。
     * */
    elements = c->argc-scoreidx;
    // 参数 member - score 对，直接报错
    if (elements % 2 || !elements) {
        addReply(c,shared.syntaxerr);
        return;
    }
    elements /= 2; /* Now this holds the number of score-element pairs. 
                    *
                    * 现在，这保存了分数元素对的数量。
                    * */

    /* Check for incompatible options. 
     *
     * 检查不兼容的选项。
     * */
    if (nx && xx) {
        addReplyError(c,
            "XX and NX options at the same time are not compatible");
        return;
    }

    if (incr && elements > 1) {
        addReplyError(c,
            "INCR option supports a single increment-element pair");
        return;
    }

    /* Start parsing all the scores, we need to emit any syntax error
     * before executing additions to the sorted set, as the command should
     * either execute fully or nothing at all. 
     *
     * 开始parse 所有输入的 score，我们需要在执行对排序集的添加之前发出任何语法错误，因为命令应
     * 该完全执行，或者什么都不执行。
     * */
    scores = zmalloc(sizeof(double)*elements);
    for (j = 0; j < elements; j++) {
        if (getDoubleFromObjectOrReply(c,c->argv[scoreidx+j*2],&scores[j],NULL)
            != C_OK) goto cleanup;
    }

    /* Lookup the key and create the sorted set if does not exist. 
     *
     * 查找关键字，如果不存在则创建排序集。
     * */
    zobj = lookupKeyWrite(c->db,key);
    // 对象不存在，创建有序集
    if (zobj == NULL) {
        if (xx) goto reply_to_client; /* No key + XX option: nothing to do. 
                                       *
                                       * 无键+XX选项：无需执行任何操作。
                                       * */
        // 创建 skiplist 编码的 zset
        if (server.zset_max_ziplist_entries == 0 ||
            server.zset_max_ziplist_value < sdslen(c->argv[scoreidx+1]->ptr))
        {
            zobj = createZsetObject();
        } else {
            // 创建 ziplist 编码的 zset
            zobj = createZsetZiplistObject();
        }
        // 添加新有序集到 db
        dbAdd(c->db,key,zobj);
    } else {
        // 对已存在对象进行类型检查
        if (zobj->type != OBJ_ZSET) {
            addReply(c,shared.wrongtypeerr);
            goto cleanup;
        }
    }

    // 遍历所有元素，将它们加入到有序集
    // O(N^4)
    for (j = 0; j < elements; j++) {
        double newscore;
        score = scores[j];
        int retflags = flags;

        ele = c->argv[scoreidx+1+j*2]->ptr;
        int retval = zsetAdd(zobj, score, ele, &retflags, &newscore);
        if (retval == 0) {
            addReplyError(c,nanerr);
            goto cleanup;
        }
        if (retflags & ZADD_ADDED) added++;
        if (retflags & ZADD_UPDATED) updated++;
        if (!(retflags & ZADD_NOP)) processed++;
        score = newscore;
    }
    server.dirty += (added+updated);

reply_to_client:
    if (incr) { /* ZINCRBY or INCR option. 
                 *
                 * ZINCRBY或INCR选项。
                 * */
        if (processed)
            addReplyDouble(c,score);
        else
            addReplyNull(c);
    } else { /* ZADD.
              * */
        addReplyLongLong(c,ch ? added+updated : added);
    }

cleanup:
    zfree(scores);
    if (added || updated) {
        signalModifiedKey(c,c->db,key);
        notifyKeyspaceEvent(NOTIFY_ZSET,
            incr ? "zincr" : "zadd", key, c->db->id);
    }
}

void zaddCommand(client *c) {
    zaddGenericCommand(c,ZADD_NONE);
}

void zincrbyCommand(client *c) {
    zaddGenericCommand(c,ZADD_INCR);
}

/*
 * 多态元素删除函数
 *
 * T = O(N^3)
 */
void zremCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    int deleted = 0, keyremoved = 0, j;

    // 查找对象，检查类型
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    for (j = 2; j < c->argc; j++) {
        if (zsetDel(zobj,c->argv[j]->ptr)) deleted++;
        if (zsetLength(zobj) == 0) {
            dbDelete(c->db,key);
            keyremoved = 1;
            break;
        }
    }

    if (deleted) {
        notifyKeyspaceEvent(NOTIFY_ZSET,"zrem",key,c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
        signalModifiedKey(c,c->db,key);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

/* Implements ZREMRANGEBYRANK, ZREMRANGEBYSCORE, ZREMRANGEBYLEX commands. 
 *
 * 实现ZREMANGEBYRANK、ZREMANGBYSCORE、ZREMAANGEBYLEX命令。
 * */
#define ZRANGE_RANK 0
#define ZRANGE_SCORE 1
#define ZRANGE_LEX 2
void zremrangeGenericCommand(client *c, int rangetype) {
    robj *key = c->argv[1];
    robj *zobj;
    int keyremoved = 0;
    unsigned long deleted = 0;
    zrangespec range;
    zlexrangespec lexrange;
    long start, end, llen;

    /* Step 1: Parse the range. 
     *
     * 步骤1：分析范围。
     * */
    if (rangetype == ZRANGE_RANK) {
        if ((getLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK) ||
            (getLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK))
            return;
    } else if (rangetype == ZRANGE_SCORE) {
        if (zslParseRange(c->argv[2],c->argv[3],&range) != C_OK) {
            addReplyError(c,"min or max is not a float");
            return;
        }
    } else if (rangetype == ZRANGE_LEX) {
        if (zslParseLexRange(c->argv[2],c->argv[3],&lexrange) != C_OK) {
            addReplyError(c,"min or max not valid string range item");
            return;
        }
    }

    /* Step 2: Lookup & range sanity checks if needed. 
     *
     * 步骤2：如果需要，查找和范围健全性检查。
     * */
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) goto cleanup;

    if (rangetype == ZRANGE_RANK) {
        /* Sanitize indexes. 
         *
         * 移出索引。
         * */
        llen = zsetLength(zobj);
        if (start < 0) start = llen+start;
        if (end < 0) end = llen+end;
        if (start < 0) start = 0;

        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. 
         *
         * 不变量：start>=0，因此当end＜0时，此测试将为true。当开始>结束或
         * 开始>=长度时，范围为空。
         * */
        if (start > end || start >= llen) {
            addReply(c,shared.czero);
            goto cleanup;
        }
        if (end >= llen) end = llen-1;
    }

    /* Step 3: Perform the range deletion operation. 
     *
     * 步骤3：执行范围删除操作。
     * */
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        switch(rangetype) {
        case ZRANGE_RANK:
            zobj->ptr = zzlDeleteRangeByRank(zobj->ptr,start+1,end+1,&deleted);
            break;
        case ZRANGE_SCORE:
            zobj->ptr = zzlDeleteRangeByScore(zobj->ptr,&range,&deleted);
            break;
        case ZRANGE_LEX:
            zobj->ptr = zzlDeleteRangeByLex(zobj->ptr,&lexrange,&deleted);
            break;
        }
        if (zzlLength(zobj->ptr) == 0) {
            dbDelete(c->db,key);
            keyremoved = 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        switch(rangetype) {
        case ZRANGE_RANK:
            deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
            break;
        case ZRANGE_SCORE:
            deleted = zslDeleteRangeByScore(zs->zsl,&range,zs->dict);
            break;
        case ZRANGE_LEX:
            deleted = zslDeleteRangeByLex(zs->zsl,&lexrange,zs->dict);
            break;
        }
        if (htNeedsResize(zs->dict)) dictResize(zs->dict);
        if (dictSize(zs->dict) == 0) {
            dbDelete(c->db,key);
            keyremoved = 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    /* Step 4: Notifications and reply. 
     *
     * 步骤4：通知和回复。
     * */
    if (deleted) {
        char *event[3] = {"zremrangebyrank","zremrangebyscore","zremrangebylex"};
        signalModifiedKey(c,c->db,key);
        notifyKeyspaceEvent(NOTIFY_ZSET,event[rangetype],key,c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
    }
    server.dirty += deleted;
    addReplyLongLong(c,deleted);

cleanup:
    if (rangetype == ZRANGE_LEX) zslFreeLexRange(&lexrange);
}

/*
 * 删除给定排序范围内的所有元素
 *
 * T = O(N^2)
 */
void zremrangebyrankCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_RANK);
}

/*
 * 多态地移除给定 score 范围内的元素
 *
 * T = O(N^2)
 */
void zremrangebyscoreCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_SCORE);
}

void zremrangebylexCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_LEX);
}

/*
 * 迭代器结构
 */
typedef struct {
    // 迭代目标
    robj *subject;
    // 类型：可以是集合或有序集
    int type; /* Set, sorted set 
               *
               * 集合，排序集合
               * */
    // 编码
    int encoding;
    // 权重
    double weight;

    union {
        /* Set iterators. 
         *
         * 设置迭代器。
         * */
        union _iterset {
            struct {
                intset *is;
                int ii;
            } is;
            struct {
                dict *dict;
                dictIterator *di;
                dictEntry *de;
            } ht;
        } set;

        /* Sorted set iterators. 
         *
         * 排序的集合迭代器。
         * */
        union _iterzset {
            // ziplist 编码
            struct {
                unsigned char *zl;
                unsigned char *eptr, *sptr;
            } zl;
            // zset 编码
            struct {
                zset *zs;
                zskiplistNode *node;
            } sl;
        } zset;
    } iter;
} zsetopsrc;


/* Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well. 
 *
 * 对于在zsetopval上的下一次迭代中需要清理的指针，请使用脏标志。长-长值的
 * 脏标志是特殊的，因为长-长的值不需要清理。相反，这意味着我们已经检查了“ell”
 * 是否包含long-long，或者试图将另一个表示转换为long-long值。如果
 * 成功，也会设置OPVAL_VALID_LL。
 * */
#define OPVAL_DIRTY_SDS 1
#define OPVAL_DIRTY_LL 2
#define OPVAL_VALID_LL 4

/* Store value retrieved from the iterator. 
 *
 * 存储从迭代器检索到的值。
 * */
typedef struct {
    int flags;
    unsigned char _buf[32]; /* Private buffer. 
                             *
                             * 专用缓冲区。
                             * */
    // 可能保存 member 的几个类型
    sds ele;
    unsigned char *estr;
    unsigned int elen;
    long long ell;
    // score 值
    double score;
} zsetopval;

typedef union _iterset iterset;
typedef union _iterzset iterzset;

/*
 * 初始化迭代器
 */
void zuiInitIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            it->is.is = op->subject->ptr;
            it->is.ii = 0;
        } else if (op->encoding == OBJ_ENCODING_HT) {
            it->ht.dict = op->subject->ptr;
            it->ht.di = dictGetIterator(op->subject->ptr);
            it->ht.de = dictNext(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            it->zl.zl = op->subject->ptr;
            it->zl.eptr = ziplistIndex(it->zl.zl,0);
            if (it->zl.eptr != NULL) {
                it->zl.sptr = ziplistNext(it->zl.zl,it->zl.eptr);
                serverAssert(it->zl.sptr != NULL);
            }
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            it->sl.zs = op->subject->ptr;
            it->sl.node = it->sl.zs->zsl->header->level[0].forward;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/*
 * 清空迭代器
 */
void zuiClearIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            UNUSED(it); /* skip 
                         *
                         * 跳过
                         * */
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dictReleaseIterator(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            UNUSED(it); /* skip 
                         *
                         * 跳过
                         * */
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            UNUSED(it); /* skip 
                         *
                         * 跳过
                         * */
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/*
 * 返回迭代对象的元素数量
 */
unsigned long zuiLength(zsetopsrc *op) {
    if (op->subject == NULL)
        return 0;

    if (op->type == OBJ_SET) {
        if (op->encoding == OBJ_ENCODING_INTSET) {
            return intsetLen(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dict *ht = op->subject->ptr;
            return dictSize(ht);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            return zzlLength(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            return zs->zsl->length;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/* Check if the current value is valid. If so, store it in the passed structure
 * and move to the next element. If not valid, this means we have reached the
 * end of the structure and can abort. 
 *
 * 检查当前值是否有效。如果是，请将其存储在传递的结构中，然后移动到下一个元素。如果
 * 无效，这意味着我们已经到达结构的末尾，可以中止。
 * */
/*
 * 从迭代器中取出下一个元素，并将它保存到 val ，然后返回 1 。
 *
 * 当没有下一个元素时，返回 0 。
 *
 * T = O(N)
 */
int zuiNext(zsetopsrc *op, zsetopval *val) {
    if (op->subject == NULL)
        return 0;

    if (val->flags & OPVAL_DIRTY_SDS)
        sdsfree(val->ele);

    // 清零
    memset(val,0,sizeof(zsetopval));

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            int64_t ell;

            if (!intsetGet(it->is.is,it->is.ii,&ell))
                return 0;
            val->ell = ell;
            val->score = 1.0;

            /* Move to next element. 
             *
             * 移动到下一个元素。
             * */
            it->is.ii++;
        } else if (op->encoding == OBJ_ENCODING_HT) {
            if (it->ht.de == NULL)
                return 0;
            val->ele = dictGetKey(it->ht.de);
            val->score = 1.0;

            /* Move to next element. 
             *
             * 移动到下一个元素。
             * */
            it->ht.de = dictNext(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            /* No need to check both, but better be explicit. 
             *
             * 不需要同时检查两者，但最好明确。
             * */
            if (it->zl.eptr == NULL || it->zl.sptr == NULL)
                return 0;
            serverAssert(ziplistGet(it->zl.eptr,&val->estr,&val->elen,&val->ell));
            val->score = zzlGetScore(it->zl.sptr);

            /* Move to next element. 
             *
             * 移动到下一个元素。
             * */
            zzlNext(it->zl.zl,&it->zl.eptr,&it->zl.sptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            if (it->sl.node == NULL)
                return 0;
            val->ele = it->sl.node->ele;
            val->score = it->sl.node->score;

            /* Move to next element. 
             *
             * 移动到下一个元素。
             * */
            it->sl.node = it->sl.node->level[0].forward;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
    return 1;
}

/*
 * 从 val 里取出并返回整数值
 *
 * T = O(1)
 */
int zuiLongLongFromValue(zsetopval *val) {
    if (!(val->flags & OPVAL_DIRTY_LL)) {
        val->flags |= OPVAL_DIRTY_LL;

        // 输入为集合时使用...
        if (val->ele != NULL) {
            if (string2ll(val->ele,sdslen(val->ele),&val->ell))
                val->flags |= OPVAL_VALID_LL;
        } else if (val->estr != NULL) {
            if (string2ll((char*)val->estr,val->elen,&val->ell))
                val->flags |= OPVAL_VALID_LL;
        } else {
            /* The long long was already set, flag as valid. 
             *
             * long-long已设置，标志为有效。
             * */
            val->flags |= OPVAL_VALID_LL;
        }
    }
    return val->flags & OPVAL_VALID_LL;
}

sds zuiSdsFromValue(zsetopval *val) {
    if (val->ele == NULL) {
        if (val->estr != NULL) {
            val->ele = sdsnewlen((char*)val->estr,val->elen);
        } else {
            val->ele = sdsfromlonglong(val->ell);
        }
        val->flags |= OPVAL_DIRTY_SDS;
    }
    return val->ele;
}

/* This is different from zuiSdsFromValue since returns a new SDS string
 * which is up to the caller to free. 
 *
 * 这与zuiSdsFromValue不同，因为它返回一个新的SDS字符串，由调用方
 * 释放。
 * */
sds zuiNewSdsFromValue(zsetopval *val) {
    if (val->flags & OPVAL_DIRTY_SDS) {
        /* We have already one to return! 
         *
         * 我们已经有一个了！
         * */
        sds ele = val->ele;
        val->flags &= ~OPVAL_DIRTY_SDS;
        val->ele = NULL;
        return ele;
    } else if (val->ele) {
        return sdsdup(val->ele);
    } else if (val->estr) {
        return sdsnewlen((char*)val->estr,val->elen);
    } else {
        return sdsfromlonglong(val->ell);
    }
}

int zuiBufferFromValue(zsetopval *val) {
    if (val->estr == NULL) {
        if (val->ele != NULL) {
            val->elen = sdslen(val->ele);
            val->estr = (unsigned char*)val->ele;
        } else {
            val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),val->ell);
            val->estr = val->_buf;
        }
    }
    return 1;
}

/* Find value pointed to by val in the source pointer to by op. When found,
 * return 1 and store its score in target. Return 0 otherwise. 
 *
 * 在op指向的源指针中查找val指向的值。找到后，返回1并将其分数存储在target中。否则返回0。
 * */
int zuiFind(zsetopsrc *op, zsetopval *val, double *score) {
    if (op->subject == NULL)
        return 0;

    if (op->type == OBJ_SET) {
        if (op->encoding == OBJ_ENCODING_INTSET) {
            if (zuiLongLongFromValue(val) &&
                intsetFind(op->subject->ptr,val->ell))
            {
                *score = 1.0;
                return 1;
            } else {
                return 0;
            }
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dict *ht = op->subject->ptr;
            zuiSdsFromValue(val);
            if (dictFind(ht,val->ele) != NULL) {
                *score = 1.0;
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        zuiSdsFromValue(val);

        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            if (zzlFind(op->subject->ptr,val->ele,score) != NULL) {
                /* Score is already set by zzlFind. 
                 *
                 * 分数已经由zzlFind设置。
                 * */
                return 1;
            } else {
                return 0;
            }
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            dictEntry *de;
            if ((de = dictFind(zs->dict,val->ele)) != NULL) {
                *score = *(double*)dictGetVal(de);
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

int zuiCompareByCardinality(const void *s1, const void *s2) {
    unsigned long first = zuiLength((zsetopsrc*)s1);
    unsigned long second = zuiLength((zsetopsrc*)s2);
    if (first > second) return 1;
    if (first < second) return -1;
    return 0;
}

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetVal(_e) == NULL ? 1.0 : *(double*)dictGetVal(_e))

/*
 * 根据 aggregate 参数所指定的模式，聚合 *target 和 val 两个值。
 */
inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. 
         *
         * 当一个变量为+inf，另一个为-inf时，添加两个双精度的结果为NaN。当这些数
         * 字相加时，我们保持结果为0.0的惯例。
         * */
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net 
         *
         * 安全网
         * */
        serverPanic("Unknown ZUNION/INTER aggregate type");
    }
}

uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);

dictType setAccumulatorDictType = {
    dictSdsHash,               /* hash function  散列函数  */
    NULL,                      /* key dup  键复制  */
    NULL,                      /* val dup  值复制 */
    dictSdsKeyCompare,         /* key compare  键比较* /
    NULL,                         /* key destructor 键析构函数 */
    NULL                       /* val destructor   val析构函数  */
};

/*
 * ZUNIONSTORE 和 ZINTERSTORE 两个命令的底层实现
 *
 * T = O(N^4)
 */
void zunionInterGenericCommand(client *c, robj *dstkey, int op) {
    int i, j;
    long setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    zsetopval zval;
    sds tmp;
    size_t maxelelen = 0, totelelen = 0;
    robj *dstobj;
    zset *dstzset;
    zskiplistNode *znode;
    int touched = 0;

    /* expect setnum input keys to be given 
     *
     * 期望给定setnum个输入键
     * */
    // 取出 setnum 参数
    if ((getLongFromObjectOrReply(c, c->argv[2], &setnum, NULL) != C_OK))
        return;

    if (setnum < 1) {
        addReplyError(c,
            "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
        return;
    }

    /* test if the expected number of keys would overflow 
     *
     * 测试预期数量的键是否会溢出
     * */
    if (setnum > c->argc-3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* read keys to be used for input 
     *
     * 用于输入的读取键
     * */
    // 保存所有 key , O(N)
    src = zcalloc(sizeof(zsetopsrc) * setnum);
    for (i = 0, j = 3; i < setnum; i++, j++) {
        // 取出 key
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (obj != NULL) {
            // key 可以是 sorted set 或者 set
            if (obj->type != OBJ_ZSET && obj->type != OBJ_SET) {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }

            // 设置
            src[i].subject = obj;
            src[i].type = obj->type;
            src[i].encoding = obj->encoding;
        } else {
            src[i].subject = NULL;
        }

        /* Default all weights to 1. 
         *
         * 将所有权重默认为1。
         * */
        src[i].weight = 1.0;
    }

    /* parse optional extra arguments 
     *
     * 解析可选的额外参数
     * */
    // 分析附加参数, O(N)
    if (j < c->argc) {
        int remaining = c->argc - j;

        // O(N)
        while (remaining) {
            // 读入所有 weight 参数, O(N)
            if (remaining >= (setnum + 1) &&
                !strcasecmp(c->argv[j]->ptr,"weights"))
            {
                j++; remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    // 将 weight 保存到 src 数组中
                    if (getDoubleFromObjectOrReply(c,c->argv[j],&src[i].weight,
                            "weight value is not a float") != C_OK)
                    {
                        zfree(src);
                        return;
                    }
                }
            } else if (remaining >= 2 &&
                       !strcasecmp(c->argv[j]->ptr,"aggregate"))
            {
                // 读取所有 aggregate 参数, O(N)
                j++; remaining--;
                if (!strcasecmp(c->argv[j]->ptr,"sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(c->argv[j]->ptr,"min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(c->argv[j]->ptr,"max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReply(c,shared.syntaxerr);
                    return;
                }
                j++; remaining--;
            } else {
                zfree(src);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* sort sets from the smallest to largest, this will improve our
     * algorithm's performance 
     *
     * 从最小到最大对集合进行排序，这将提高我们算法的性能
     * */
    qsort(src,setnum,sizeof(zsetopsrc),zuiCompareByCardinality);

    dstobj = createZsetObject();
    dstzset = dstobj->ptr;
    memset(&zval, 0, sizeof(zval));

    if (op == SET_OP_INTER) {
        /* Skip everything if the smallest input is empty. 
         *
         * 如果最小的输入为空，则跳过所有内容。
         * */
        if (zuiLength(&src[0]) > 0) {
            /* Precondition: as src[0] is non-empty and the inputs are ordered
             * by size, all src[i > 0] are non-empty too. 
             *
             * 前提条件：由于src[0]不为空，并且输入按大小排序，因此所有src[i>0]也
             * 不为空。
             * */
            zuiInitIterator(&src[0]);
            while (zuiNext(&src[0],&zval)) {
                double score, value;

                score = src[0].weight * zval.score;
                if (isnan(score)) score = 0;

                for (j = 1; j < setnum; j++) {
                    /* It is not safe to access the zset we are
                     * iterating, so explicitly check for equal object. 
                     *
                     * 访问我们正在迭代的zset是不安全的，所以显式地检查equal对象。
                     * */
                    if (src[j].subject == src[0].subject) {
                        value = zval.score*src[j].weight;
                        zunionInterAggregate(&score,value,aggregate);
                    } else if (zuiFind(&src[j],&zval,&value)) {
                        value *= src[j].weight;
                        zunionInterAggregate(&score,value,aggregate);
                    } else {
                        break;
                    }
                }

                /* Only continue when present in every input. 
                 *
                 * 仅当存在于每个输入中时才继续。
                 * */
                if (j == setnum) {
                    tmp = zuiNewSdsFromValue(&zval);
                    znode = zslInsert(dstzset->zsl,score,tmp);
                    dictAdd(dstzset->dict,tmp,&znode->score);
                    totelelen += sdslen(tmp);
                    if (sdslen(tmp) > maxelelen) maxelelen = sdslen(tmp);
                }
            }
            zuiClearIterator(&src[0]);
        }
    } else if (op == SET_OP_UNION) {
        dict *accumulator = dictCreate(&setAccumulatorDictType,NULL);
        dictIterator *di;
        dictEntry *de, *existing;
        double score;

        if (setnum) {
            /* Our union is at least as large as the largest set.
             * Resize the dictionary ASAP to avoid useless rehashing. 
             *
             * 我们的联合至少和最大的联合一样大。尽快调整字典大小以避免无用的重洗。
             * */
            dictExpand(accumulator,zuiLength(&src[setnum-1]));
        }

        /* Step 1: Create a dictionary of elements -> aggregated-scores
         * by iterating one sorted set after the other. 
         *
         * 步骤1：创建一个元素字典->通过一个接一个地迭代一个排序集来聚合分数。
         * */
        for (i = 0; i < setnum; i++) {
            if (zuiLength(&src[i]) == 0) continue;

            zuiInitIterator(&src[i]);
            while (zuiNext(&src[i],&zval)) {
                /* Initialize value 
                 *
                 * 初始化值
                 * */
                score = src[i].weight * zval.score;
                if (isnan(score)) score = 0;

                /* Search for this element in the accumulating dictionary. 
                 *
                 * 在累积字典中搜索此元素。
                 * */
                de = dictAddRaw(accumulator,zuiSdsFromValue(&zval),&existing);
                /* If we don't have it, we need to create a new entry. 
                 *
                 * 如果没有，我们需要创建一个新节点。
                 * */
                if (!existing) {
                    tmp = zuiNewSdsFromValue(&zval);
                    /* Remember the longest single element encountered,
                     * to understand if it's possible to convert to ziplist
                     * at the end. 
                     *
                     * 记住遇到的最长的单个元素，以了解最后是否可以转换为ziplist。
                     * */
                     totelelen += sdslen(tmp);
                     if (sdslen(tmp) > maxelelen) maxelelen = sdslen(tmp);
                    /* Update the element with its initial score. 
                     *
                     * 使用元素的初始分数更新该元素。
                     * */
                    dictSetKey(accumulator, de, tmp);
                    dictSetDoubleVal(de,score);
                } else {
                    /* Update the score with the score of the new instance
                     * of the element found in the current sorted set.
                     *
                     * Here we access directly the dictEntry double
                     * value inside the union as it is a big speedup
                     * compared to using the getDouble/setDouble API. 
                     *
                     * 使用在当前排序集中找到的元素的新实例的分数更新分数。在这里，我们直接访问union中的dictEntry double值，
                     * 因为与使用getDouble/setDouble API相比，这是一个很大的加速。
                     * */
                    zunionInterAggregate(&existing->v.d,score,aggregate);
                }
            }
            zuiClearIterator(&src[i]);
        }

        /* Step 2: convert the dictionary into the final sorted set. 
         *
         * 步骤2：将字典转换为最终排序的集合。
         * */
        di = dictGetIterator(accumulator);

        /* We now are aware of the final size of the resulting sorted set,
         * let's resize the dictionary embedded inside the sorted set to the
         * right size, in order to save rehashing time. 
         *
         * 我们现在已经知道了最终排序集的大小，让我们将嵌入排序集中的字典调整到合适的大小，
         * 以节省重新哈希的时间。
         * */
        dictExpand(dstzset->dict,dictSize(accumulator));

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            score = dictGetDoubleVal(de);
            znode = zslInsert(dstzset->zsl,score,ele);
            dictAdd(dstzset->dict,ele,&znode->score);
        }
        dictReleaseIterator(di);
        dictRelease(accumulator);
    } else {
        serverPanic("Unknown operator");
    }

    if (dbDelete(c->db,dstkey))
        touched = 1;
    if (dstzset->zsl->length) {
        zsetConvertToZiplistIfNeeded(dstobj,maxelelen,totelelen);
        dbAdd(c->db,dstkey,dstobj);
        addReplyLongLong(c,zsetLength(dstobj));
        signalModifiedKey(c,c->db,dstkey);
        notifyKeyspaceEvent(NOTIFY_ZSET,
            (op == SET_OP_UNION) ? "zunionstore" : "zinterstore",
            dstkey,c->db->id);
        server.dirty++;
    } else {
        decrRefCount(dstobj);
        addReply(c,shared.czero);
        if (touched) {
            signalModifiedKey(c,c->db,dstkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            server.dirty++;
        }
    }
    zfree(src);
}

void zunionstoreCommand(client *c) {
    zunionInterGenericCommand(c,c->argv[1], SET_OP_UNION);
}

void zinterstoreCommand(client *c) {
    zunionInterGenericCommand(c,c->argv[1], SET_OP_INTER);
}

/*
 * T = O(N)
 * */
void zrangeGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *zobj;
    int withscores = 0;
    long start;
    long end;
    long llen;
    long rangelen;

    // 取出 start 和 end 参数
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    // 取出 withscores 参数
    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) {
        withscores = 1;
    } else if (c->argc >= 5) {
        addReply(c,shared.syntaxerr);
        return;
    }

    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptyarray)) == NULL
         || checkType(c,zobj,OBJ_ZSET)) return;

    /* Sanitize indexes. 
     *
     * 去除索引。
     * */
    llen = zsetLength(zobj);
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. 
     *
     * 不变量：start>=0，因此当end＜0时，此测试将为true。当开始>结束或开始>=长度时，范围为空。
     * */
    if (start > end || start >= llen) {
        addReply(c,shared.emptyarray);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply. RESP3 clients
     * will receive sub arrays with score->element, while RESP2 returned
     * a flat array. 
     *
     * 以多批量回复的形式返回结果。RESP3客户端将接收带有score->元素的子数组
     * ，而RESP2返回一个平面数组。
     * */
    if (withscores && c->resp == 2)
        addReplyArrayLen(c, rangelen*2);
    else
        addReplyArrayLen(c, rangelen);

    // ziplist 编码, O(N)
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        // 决定迭代的方向
        // 并指向第一个 member
        // O(1)
        if (reverse)
            eptr = ziplistIndex(zl,-2-(2*start));
        else
            eptr = ziplistIndex(zl,2*start);

        serverAssertWithInfo(c,zobj,eptr != NULL);
        // 指向第一个 score
        sptr = ziplistNext(zl,eptr);

        // 取出元素, O(N)
        while (rangelen--) {
            // 元素不为空
            serverAssertWithInfo(c,zobj,eptr != NULL && sptr != NULL);
            // 取出 member
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));

            // 取出 score
            if (withscores && c->resp > 2) addReplyArrayLen(c,2);
            if (vstr == NULL)
                addReplyBulkLongLong(c,vlong);
            else
                addReplyBulkCBuffer(c,vstr,vlen);
            if (withscores) addReplyDouble(c,zzlGetScore(sptr));

            // 移动指针到下一个节点
            if (reverse)
                zzlPrev(zl,&eptr,&sptr);
            else
                zzlNext(zl,&eptr,&sptr);
        }

    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        sds ele;

        /* Check if starting point is trivial, before doing log(N) lookup. 
         *
         * 在进行log（N）查找之前，请检查起点是否微不足道。
         * */
        // 决定起始节点, O(N)
        if (reverse) {
            ln = zsl->tail;
            if (start > 0)
                ln = zslGetElementByRank(zsl,llen-start);
        } else {
            ln = zsl->header->level[0].forward;
            if (start > 0)
                ln = zslGetElementByRank(zsl,start+1);
        }

        while(rangelen--) {
            serverAssertWithInfo(c,zobj,ln != NULL);
            // 返回 member
            ele = ln->ele;
            // 返回 score
            if (withscores && c->resp > 2) addReplyArrayLen(c,2);
            addReplyBulkCBuffer(c,ele,sdslen(ele));
            if (withscores) addReplyDouble(c,ln->score);
            ln = reverse ? ln->backward : ln->level[0].forward;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

void zrangeCommand(client *c) {
    zrangeGenericCommand(c,0);
}

void zrevrangeCommand(client *c) {
    zrangeGenericCommand(c,1);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE. 
 *
 * 此命令实现ZRANGEBYSCORE、ZREVRANGEBYSCORE。
 * */
void genericZrangebyscoreCommand(client *c, int reverse) {
    zrangespec range;
    robj *key = c->argv[1];
    robj *zobj;
    long offset = 0, limit = -1;
    int withscores = 0;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. 
     *
     * 分析范围参数。
     * */
    if (reverse) {
        /* Range is given as [max,min] 
         *
         * 范围为[max，min]
         * */
        maxidx = 2; minidx = 3;
    } else {
        /* Range is given as [min,max] 
         *
         * 范围为[min，max]
         * */
        minidx = 2; maxidx = 3;
    }

    if (zslParseRange(c->argv[minidx],c->argv[maxidx],&range) != C_OK) {
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. 
     *
     * 分析可选的额外参数。请注意，ZCOUNT正好有4个参数，因此我们永远不会输入以下
     * 代码路径。
     * */
    if (c->argc > 4) {
        int remaining = c->argc - 4;
        int pos = 4;

        while (remaining) {
            if (remaining >= 1 && !strcasecmp(c->argv[pos]->ptr,"withscores")) {
                pos++; remaining--;
                withscores = 1;
            } else if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL)
                        != C_OK) ||
                    (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL)
                        != C_OK))
                {
                    return;
                }
                pos += 3; remaining -= 3;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range 
     *
     * 好的，查找关键字并获取范围
     * */
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptyarray)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        double score;

        /* If reversed, get the last node in range as starting point. 
         *
         * 如果相反，则获取范围中的最后一个节点作为起点。
         * */
        if (reverse) {
            eptr = zzlLastInRange(zl,&range);
        } else {
            eptr = zzlFirstInRange(zl,&range);
        }

        /* No "first" element in the specified interval. 
         *
         * 在指定的间隔中没有“第一个”元素。
         * */
        if (eptr == NULL) {
            addReply(c,shared.emptyarray);
            return;
        }

        /* Get score pointer for the first element. 
         *
         * 获取第一个元素的分数指针。
         * */
        serverAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later 
         *
         * 我们事先不知道列表中有多少匹配元素，所以我们在输出缓冲区中推送这个表示多批量长度
         * 的对象，稍后会“修复”它
         * */
        replylen = addReplyDeferredLen(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. 
         *
         * 如果有偏移，只需遍历元素的数量，而不检查分数，因为这是在下一个循环中完成的。
         * */
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }

        while (eptr && limit--) {
            score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. 
             *
             * 当节点不再在范围内时中止。
             * */
            if (reverse) {
                if (!zslValueGteMin(score,&range)) break;
            } else {
                if (!zslValueLteMax(score,&range)) break;
            }

            /* We know the element exists, so ziplistGet should always
             * succeed 
             *
             * 我们知道元素存在，所以ziplistGet应该总是成功的
             * */
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));

            rangelen++;
            if (withscores && c->resp > 2) addReplyArrayLen(c,2);
            if (vstr == NULL) {
                addReplyBulkLongLong(c,vlong);
            } else {
                addReplyBulkCBuffer(c,vstr,vlen);
            }
            if (withscores) addReplyDouble(c,score);

            /* Move to next node 
             *
             * 移动到下一个节点
             * */
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. 
         *
         * 如果相反，则获取范围中的最后一个节点作为起点。
         * */
        if (reverse) {
            ln = zslLastInRange(zsl,&range);
        } else {
            ln = zslFirstInRange(zsl,&range);
        }

        /* No "first" element in the specified interval. 
         *
         * 在指定的间隔中没有“第一个”元素。
         * */
        if (ln == NULL) {
            addReply(c,shared.emptyarray);
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later 
         *
         * 我们事先不知道列表中有多少匹配元素，所以我们在输出缓冲区中推送这个表示多批量长度
         * 的对象，稍后会“修复”它
         * */
        replylen = addReplyDeferredLen(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. 
         *
         * 如果有偏移，只需遍历元素的数量，而不检查分数，因为这是在下一个循环中完成的。
         * */
        while (ln && offset--) {
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }

        while (ln && limit--) {
            /* Abort when the node is no longer in range. 
             *
             * 当节点不再在范围内时中止。
             * */
            if (reverse) {
                if (!zslValueGteMin(ln->score,&range)) break;
            } else {
                if (!zslValueLteMax(ln->score,&range)) break;
            }

            rangelen++;
            if (withscores && c->resp > 2) addReplyArrayLen(c,2);
            addReplyBulkCBuffer(c,ln->ele,sdslen(ln->ele));
            if (withscores) addReplyDouble(c,ln->score);

            /* Move to next node 
             *
             * 移动到下一个节点
             * */
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    if (withscores && c->resp == 2) rangelen *= 2;
    setDeferredArrayLen(c, replylen, rangelen);
}

void zrangebyscoreCommand(client *c) {
    genericZrangebyscoreCommand(c,0);
}

void zrevrangebyscoreCommand(client *c) {
    genericZrangebyscoreCommand(c,1);
}

/*
 * 返回有序集在给定范围内的元素
 *
 * T = O(N)
 */
void zcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zrangespec range;
    unsigned long count = 0;

    /* Parse the range arguments 
     *
     * 分析范围参数
     * */
    // range 参数
    if (zslParseRange(c->argv[2],c->argv[3],&range) != C_OK) {
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Lookup the sorted set 
     *
     * 查找已排序的集合
     * */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL ||
        checkType(c, zobj, OBJ_ZSET)) return;

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        double score;

        /* Use the first element in range as the starting point 
         *
         * 使用范围中的第一个元素作为起点
         * */
        eptr = zzlFirstInRange(zl,&range);

        /* No "first" element 
         *
         * 没有“第一个”元素
         * */
        if (eptr == NULL) {
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range 
         *
         * 第一个元素在范围内
         * */
        sptr = ziplistNext(zl,eptr);
        score = zzlGetScore(sptr);
        serverAssertWithInfo(c,zobj,zslValueLteMax(score,&range));

        /* Iterate over elements in range 
         *
         * 在范围内的元素上迭代
         * */
        while (eptr) {
            score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. 
             *
             * 当节点不再在范围内时中止。
             * */
            if (!zslValueLteMax(score,&range)) {
                break;
            } else {
                count++;
                zzlNext(zl,&eptr,&sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range 
         *
         * 查找范围中的第一个元素
         * */
        zn = zslFirstInRange(zsl, &range);

        /* Use rank of first element, if any, to determine preliminary count 
         *
         * 使用第一个元素的秩（如果有）来确定初步计数
         * */
        if (zn != NULL) {
            rank = zslGetRank(zsl, zn->score, zn->ele);
            count = (zsl->length - (rank - 1));

            /* Find last element in range 
             *
             * 查找范围中的最后一个元素
             * */
            zn = zslLastInRange(zsl, &range);

            /* Use rank of last element, if any, to determine the actual count 
             *
             * 使用最后一个元素的秩（如果有）来确定实际计数
             * */
            if (zn != NULL) {
                rank = zslGetRank(zsl, zn->score, zn->ele);
                count -= (zsl->length - rank);
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    addReplyLongLong(c, count);
}

void zlexcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zlexrangespec range;
    unsigned long count = 0;

    /* Parse the range arguments 
     *
     * 分析范围参数
     * */
    if (zslParseLexRange(c->argv[2],c->argv[3],&range) != C_OK) {
        addReplyError(c,"min or max not valid string range item");
        return;
    }

    /* Lookup the sorted set 
     *
     * 查找已排序的集合
     * */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL ||
        checkType(c, zobj, OBJ_ZSET))
    {
        zslFreeLexRange(&range);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        /* Use the first element in range as the starting point 
         *
         * 使用范围中的第一个元素作为起点
         * */
        eptr = zzlFirstInLexRange(zl,&range);

        /* No "first" element 
         *
         * 没有“第一个”元素
         * */
        if (eptr == NULL) {
            zslFreeLexRange(&range);
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range 
         *
         * 第一个元素在范围内
         * */
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(c,zobj,zzlLexValueLteMax(eptr,&range));

        /* Iterate over elements in range 
         *
         * 在范围内的元素上迭代
         * */
        while (eptr) {
            /* Abort when the node is no longer in range. 
             *
             * 当节点不再在范围内时中止。
             * */
            if (!zzlLexValueLteMax(eptr,&range)) {
                break;
            } else {
                count++;
                zzlNext(zl,&eptr,&sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range 
         *
         * 查找范围中的第一个元素
         * */
        zn = zslFirstInLexRange(zsl, &range);

        /* Use rank of first element, if any, to determine preliminary count 
         *
         * 使用第一个元素的秩（如果有）来确定初步计数
         * */
        if (zn != NULL) {
            // 查找元素在跳跃表中的位置 O(N)
            rank = zslGetRank(zsl, zn->score, zn->ele);
            count = (zsl->length - (rank - 1));

            /* Find last element in range 
             *
             * 查找范围中的最后一个元素
             * */
            zn = zslLastInLexRange(zsl, &range);

            /* Use rank of last element, if any, to determine the actual count 
             *
             * 使用最后一个元素的秩（如果有）来确定实际计数
             * */
            if (zn != NULL) {
                rank = zslGetRank(zsl, zn->score, zn->ele);
                count -= (zsl->length - rank);
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zslFreeLexRange(&range);
    addReplyLongLong(c, count);
}

/* This command implements ZRANGEBYLEX, ZREVRANGEBYLEX. 
 *
 * 此命令实现ZRANGEBYLEX、ZREVRANGEBYLEX。
 * */
void genericZrangebylexCommand(client *c, int reverse) {
    zlexrangespec range;
    robj *key = c->argv[1];
    robj *zobj;
    long offset = 0, limit = -1;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. 
     *
     * 分析范围参数。
     * */
    if (reverse) {
        /* Range is given as [max,min] 
         *
         * 范围为[max，min]
         * */
        maxidx = 2; minidx = 3;
    } else {
        /* Range is given as [min,max] 
         *
         * 范围为[min，max]
         * */
        minidx = 2; maxidx = 3;
    }

    if (zslParseLexRange(c->argv[minidx],c->argv[maxidx],&range) != C_OK) {
        addReplyError(c,"min or max not valid string range item");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. 
     *
     * 分析可选的额外参数。请注意，ZCOUNT正好有4个参数，因此我们永远不会输入以下
     * 代码路径。
     * */
    if (c->argc > 4) {
        int remaining = c->argc - 4;
        int pos = 4;

        while (remaining) {
            if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != C_OK) ||
                    (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != C_OK)) {
                    zslFreeLexRange(&range);
                    return;
                }
                pos += 3; remaining -= 3;
            } else {
                zslFreeLexRange(&range);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range 
     *
     * 好的，查找关键字并获取范围
     * */
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptyarray)) == NULL ||
        checkType(c,zobj,OBJ_ZSET))
    {
        zslFreeLexRange(&range);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        /* If reversed, get the last node in range as starting point. 
         *
         * 如果相反，则获取范围中的最后一个节点作为起点。
         * */
        if (reverse) {
            eptr = zzlLastInLexRange(zl,&range);
        } else {
            eptr = zzlFirstInLexRange(zl,&range);
        }

        /* No "first" element in the specified interval. 
         *
         * 在指定的间隔中没有“第一个”元素。
         * */
        if (eptr == NULL) {
            addReply(c,shared.emptyarray);
            zslFreeLexRange(&range);
            return;
        }

        /* Get score pointer for the first element. 
         *
         * 获取第一个元素的分数指针。
         * */
        serverAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later 
         *
         * 我们事先不知道列表中有多少匹配元素，所以我们在输出缓冲区中推送这个表示多批量长度
         * 的对象，稍后会“修复”它
         * */
        replylen = addReplyDeferredLen(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. 
         *
         * 如果有偏移，只需遍历元素的数量，而不检查分数，因为这是在下一个循环中完成的。
         * */
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }

        while (eptr && limit--) {
            /* Abort when the node is no longer in range. 
             *
             * 当节点不再在范围内时中止。
             * */
            if (reverse) {
                if (!zzlLexValueGteMin(eptr,&range)) break;
            } else {
                if (!zzlLexValueLteMax(eptr,&range)) break;
            }

            /* We know the element exists, so ziplistGet should always
             * succeed. 
             *
             * 我们知道元素存在，所以ziplistGet应该总是成功的。
             * */
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));

            rangelen++;
            if (vstr == NULL) {
                addReplyBulkLongLong(c,vlong);
            } else {
                addReplyBulkCBuffer(c,vstr,vlen);
            }

            /* Move to next node 
             *
             * 移动到下一个节点
             * */
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. 
         *
         * 如果相反，则获取范围中的最后一个节点作为起点。
         * */
        if (reverse) {
            ln = zslLastInLexRange(zsl,&range);
        } else {
            ln = zslFirstInLexRange(zsl,&range);
        }

        /* No "first" element in the specified interval. 
         *
         * 在指定的间隔中没有“第一个”元素。
         * */
        if (ln == NULL) {
            addReply(c,shared.emptyarray);
            zslFreeLexRange(&range);
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later 
         *
         * 我们事先不知道列表中有多少匹配元素，所以我们在输出缓冲区中推送这个表示多批量长度的对象，稍后会“修复”它
         * */
        replylen = addReplyDeferredLen(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. 
         *
         * 如果有偏移，只需遍历元素的数量，而不检查分数，因为这是在下一个循环中完成的。
         * */
        while (ln && offset--) {
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }

        while (ln && limit--) {
            /* Abort when the node is no longer in range. 
             *
             * 当节点不再在范围内时中止。
             * */
            if (reverse) {
                if (!zslLexValueGteMin(ln->ele,&range)) break;
            } else {
                if (!zslLexValueLteMax(ln->ele,&range)) break;
            }

            rangelen++;
            addReplyBulkCBuffer(c,ln->ele,sdslen(ln->ele));

            /* Move to next node 
             *
             * 移动到下一个节点
             * */
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zslFreeLexRange(&range);
    setDeferredArrayLen(c, replylen, rangelen);
}

void zrangebylexCommand(client *c) {
    genericZrangebylexCommand(c,0);
}

void zrevrangebylexCommand(client *c) {
    genericZrangebylexCommand(c,1);
}

void zcardCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    addReplyLongLong(c,zsetLength(zobj));
}

void zscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    double score;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.null[c->resp])) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    if (zsetScore(zobj,c->argv[2]->ptr,&score) == C_ERR) {
        addReplyNull(c);
    } else {
        addReplyDouble(c,score);
    }
}

void zrankGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *ele = c->argv[2];
    robj *zobj;
    long rank;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.null[c->resp])) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    serverAssertWithInfo(c,ele,sdsEncodedObject(ele));
    rank = zsetRank(zobj,ele->ptr,reverse);
    if (rank >= 0) {
        addReplyLongLong(c,rank);
    } else {
        addReplyNull(c);
    }
}

void zrankCommand(client *c) {
    zrankGenericCommand(c, 0);
}

void zrevrankCommand(client *c) {
    zrankGenericCommand(c, 1);
}

void zscanCommand(client *c) {
    robj *o;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_ZSET)) return;
    scanGenericCommand(c,o,cursor);
}

/* This command implements the generic zpop operation, used by:
 * ZPOPMIN, ZPOPMAX, BZPOPMIN and BZPOPMAX. This function is also used
 * inside blocked.c in the unblocking stage of BZPOPMIN and BZPOPMAX.
 *
 * If 'emitkey' is true also the key name is emitted, useful for the blocking
 * behavior of BZPOP[MIN|MAX], since we can block into multiple keys.
 *
 * The synchronous version instead does not need to emit the key, but may
 * use the 'count' argument to return multiple items if available. */
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, robj *countarg) {
    int idx;
    robj *key = NULL;
    robj *zobj = NULL;
    sds ele;
    double score;
    long count = 1;

    /* If a count argument as passed, parse it or return an error. 
     *
     * 如果传递了count参数，请对其进行解析或返回错误。
     * */
    if (countarg) {
        if (getLongFromObjectOrReply(c,countarg,&count,NULL) != C_OK)
            return;
        if (count <= 0) {
            addReply(c,shared.emptyarray);
            return;
        }
    }

    /* Check type and break on the first error, otherwise identify candidate. 
     *
     * 检查类型并在第一个错误处中断，否则识别候选者。
     * */
    idx = 0;
    while (idx < keyc) {
        key = keyv[idx++];
        zobj = lookupKeyWrite(c->db,key);
        if (!zobj) continue;
        if (checkType(c,zobj,OBJ_ZSET)) return;
        break;
    }

    /* No candidate for zpopping, return empty. 
     *
     * 没有zpopping的候选项，返回为空。
     * */
    if (!zobj) {
        addReply(c,shared.emptyarray);
        return;
    }

    void *arraylen_ptr = addReplyDeferredLen(c);
    long result_count = 0;

    /* We emit the key only for the blocking variant. 
     *
     * 我们只为阻塞变体发出键。
     * */
    if (emitkey) addReplyBulk(c,key);

    /* Respond with a single (flat) array in RESP2 or if countarg is not
     * provided (returning a single element). In RESP3, when countarg is
     * provided, use nested array.  
     *
     * 在RESP2中使用单个（平面）数组响应，或者如果未提供countarg（返回单个元素）。在RESP3中，当提供countarg时，使用嵌套数组。
     * */
    int use_nested_array = c->resp > 2 && countarg != NULL;

    /* Remove the element. 
     *
     * 移出元素
     * */
    do {
        if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
            unsigned char *zl = zobj->ptr;
            unsigned char *eptr, *sptr;
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;

            /* Get the first or last element in the sorted set. 
             *
             * 获取排序集中的第一个或最后一个元素。
             * */
            eptr = ziplistIndex(zl,where == ZSET_MAX ? -2 : 0);
            serverAssertWithInfo(c,zobj,eptr != NULL);
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen(vstr,vlen);

            /* Get the score. 
             *
             * 记下分数。
             * */
            sptr = ziplistNext(zl,eptr);
            serverAssertWithInfo(c,zobj,sptr != NULL);
            score = zzlGetScore(sptr);
        } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = zobj->ptr;
            zskiplist *zsl = zs->zsl;
            zskiplistNode *zln;

            /* Get the first or last element in the sorted set. 
             *
             * 获取排序集中的第一个或最后一个元素。
             * */
            zln = (where == ZSET_MAX ? zsl->tail :
                                       zsl->header->level[0].forward);

            /* There must be an element in the sorted set. 
             *
             * 排序集中必须有一个元素。
             * */
            serverAssertWithInfo(c,zobj,zln != NULL);
            ele = sdsdup(zln->ele);
            score = zln->score;
        } else {
            serverPanic("Unknown sorted set encoding");
        }

        serverAssertWithInfo(c,zobj,zsetDel(zobj,ele));
        server.dirty++;

        if (result_count == 0) { /* Do this only for the first iteration. 
                                  *
                                  * 只对第一次迭代执行此操作。
                                  * */
            char *events[2] = {"zpopmin","zpopmax"};
            notifyKeyspaceEvent(NOTIFY_ZSET,events[where],key,c->db->id);
            signalModifiedKey(c,c->db,key);
        }

        if (use_nested_array) {
            addReplyArrayLen(c,2);
        }
        addReplyBulkCBuffer(c,ele,sdslen(ele));
        addReplyDouble(c,score);
        sdsfree(ele);
        ++result_count;

        /* Remove the key, if indeed needed. 
         *
         * 如果确实需要，取下键。
         * */
        if (zsetLength(zobj) == 0) {
            dbDelete(c->db,key);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
            break;
        }
    } while(--count);

    if (!use_nested_array) {
        result_count *= 2;
    }
    setDeferredArrayLen(c,arraylen_ptr,result_count + (emitkey != 0));
}

/* ZPOPMIN key [<count>] 
 *
 * ZPOPMIN键[<count>]
 * */
void zpopminCommand(client *c) {
    if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }
    genericZpopCommand(c,&c->argv[1],1,ZSET_MIN,0,
        c->argc == 3 ? c->argv[2] : NULL);
}

/* ZMAXPOP key [<count>] 
 *
 * ZMAXPOP键[<count>]
 * */
void zpopmaxCommand(client *c) {
    if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }
    genericZpopCommand(c,&c->argv[1],1,ZSET_MAX,0,
        c->argc == 3 ? c->argv[2] : NULL);
}

/* BZPOPMIN / BZPOPMAX actual implementation. 
 *
 * BZPOPMIN。BZPOPMAX实际实现。
 * */
void blockingGenericZpopCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_ZSET) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                if (zsetLength(o) != 0) {
                    /* Non empty zset, this is like a normal ZPOP[MIN|MAX]. */
                    genericZpopCommand(c,&c->argv[j],1,where,1,NULL);
                    /* Replicate it as an ZPOP[MIN|MAX] instead of BZPOP[MIN|MAX]. */
                    rewriteClientCommandVector(c,2,
                        where == ZSET_MAX ? shared.zpopmax : shared.zpopmin,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the zset is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). 
     *
     * 如果我们在MULTI/EXEC中，并且zset为空，那么我们唯一能做的就是将其视
     * 为超时（即使超时为0）。
     * */
    if (c->flags & CLIENT_MULTI) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block 
     *
     * 如果键不存在，我们必须阻止
     * */
    blockForKeys(c,BLOCKED_ZSET,c->argv + 1,c->argc - 2,timeout,NULL,NULL);
}

// BZPOPMIN key [key ...] timeout
void bzpopminCommand(client *c) {
    blockingGenericZpopCommand(c,ZSET_MIN);
}

// BZPOPMAX key [key ...] timeout
void bzpopmaxCommand(client *c) {
    blockingGenericZpopCommand(c,ZSET_MAX);
}
