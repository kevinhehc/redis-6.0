/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * listRelease(), but private value of every node need to be freed
 * by the user before to call listRelease(), or by setting a free method using
 * listSetFreeMethod.
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */

// 创建一个新列表
// 创建成功时返回列表，创建失败返回 NULL
list *listCreate(void)
{
    struct list *list;

    // 为列表结构分配内存
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;

    // 初始化属性
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
void listEmpty(list *list)
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;
    len = list->len;
    while(len--) {
        next = current->next;
        // 如果列表有自带的 free 方法，那么先对节点值调用它
        if (list->free) list->free(current->value);
        // 之后再释放节点
        zfree(current);
        current = next;
    }
    list->head = list->tail = NULL;
    list->len = 0;
}

/* Free the whole list.
 *
 * This function can't fail. */

//  释放整个列表(以及列表包含的节点)
// T = O(N)，N 为列表的长度
void listRelease(list *list)
{
    listEmpty(list);
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/*
 * 新建一个包含给定 value 的节点，并将它加入到列表的表头
 *
 * 出错时，返回 NULL ，不执行动作。
 * 成功时，返回传入的列表
 *
 * T = O(1)
 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        // 第一个节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        // 不是第一个节点
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/*
 * 新建一个包含给定 value 的节点，并将它加入到列表的表尾
 *
 * 出错时，返回 NULL ，不执行动作。
 * 成功时，返回传入的列表
 *
 * T = O(1)
 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (list->len == 0) {
        // 第一个节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        // 不是第一个节点
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}


/*
 * 创建一个包含值 value 的节点
 * 并根据 after 参数的指示，将新节点插入到 old_node 的之前或者之后
 *
 * T = O(1)
 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (after) {
        // 插入到 old_node 之后
        node->prev = old_node;
        node->next = old_node->next;
        // 处理表尾节点
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
        // 插入到 old_node 之前
        node->next = old_node;
        node->prev = old_node->prev;
        // 处理表头节点
        if (list->head == old_node) {
            list->head = node;
        }
    }
    // 更新前置节点和后继节点的指针
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    // 更新列表节点数量
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
/*
 * 释放列表中给定的节点
 * 清除节点私有值(private value)的工作由调用者完成
 *
 * T = O(1)
 */
void listDelNode(list *list, listNode *node)
{
    // 处理前驱节点的指针
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;

    // 处理后继节点的指针
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;

    // 释放节点值
    if (list->free) list->free(node->value);

    // 释放节点
    zfree(node);

    // 更新列表节点数量
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
/*
* 创建列表 list 的一个迭代器，迭代方向由参数 direction 决定
*
* 每次对迭代器调用 listNext() ，迭代器就返回列表的下一个节点
*
* 这个函数不处理失败情形
*
* T = O(1)
*/
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;

    // 根据迭代的方向，将迭代器的指针指向表头或者表尾
    if (direction == AL_START_HEAD)
        iter->next = list->head;
    else
        iter->next = list->tail;

    // 记录方向
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
/*
 * 释放迭代器 iter
 *
 * T = O(1)
 */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
/*
 * 将迭代器 iter 的迭代指针倒回 list 的表头
 *
 * T = O(1)
 */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

/*
 * 将迭代器 iter 的迭代指针倒回 list 的表尾
 *
 * T = O(1)
 */
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage
 * pattern is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
/*
 * 返回迭代器的当前节点
 *
 * 可以使用 listDelNode() 删除当前节点，但是不可以删除其他节点。
 *
 * 函数要么返回当前节点，要么返回 NULL ，因此，常见的用法是：
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * T = O(1)
 */
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
/*
 * 复制整个列表，成功返回列表的副本，内存不足而失败时返回 NULL 。
 *
 * 无论复制是成功或失败，输入列表都不会被修改。
 *
 * T = O(N)，N 为 orig 列表的长度
 */
list *listDup(list *orig)
{
    list *copy;
    listIter iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
    // 复制属性
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    // 复制节点
    listRewind(orig, &iter);
    while((node = listNext(&iter)) != NULL) {
        // 复制节点值
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->value;

        // 将新节点添加到新列表末尾
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
/*
 * 在列表中查找和 key 匹配的节点。
 *
 * 如果列表带有匹配器，那么匹配通过匹配器来进行。
 * 如果列表没有匹配器，那么直接将 key 和节点的值进行比对。
 *
 * 匹配从表头开始，第一个匹配成功的节点会被返回
 * 如果匹配不成功，返回 NULL 。
 *
 * T = O(N)，N 为列表的长度
 */
listNode *listSearchKey(list *list, void *key)
{
    listIter iter;
    listNode *node;

    // 使用迭代器查找
    listRewind(list, &iter);
    while((node = listNext(&iter)) != NULL) {
        if (list->match) {
            // 使用列表自带的匹配器进行比对
            if (list->match(node->value, key)) {
                return node;
            }
        } else {
            // 直接用列表的值来比对
            if (key == node->value) {
                return node;
            }
        }
    }
    // 没找到
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
/*
 * 根据给定索引，返回列表中对应的节点
 *
 * 索引可以是正数，也可以是负数。
 * 正数从 0 开始计数，由表头开始；负数从 -1 开始计数，由表尾开始。
 *
 * 如果给定索引超出列表的返回，返回 NULL 。
 *
 * T = O(N)，N 为列表的长度
 */
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1) return;

    /* Detach current tail */
    listNode *tail = list->tail;
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}

/* Rotate the list removing the head node and inserting it to the tail. */
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1) return;

    listNode *head = list->head;
    /* Detach current head */
    list->head = head->next;
    list->head->prev = NULL;
    /* Move it as tail */
    list->tail->next = head;
    head->next = NULL;
    head->prev = list->tail;
    list->tail = head;
}

/* Add all the elements of the list 'o' at the end of the
 * list 'l'. The list 'other' remains empty but otherwise valid. */
void listJoin(list *l, list *o) {
    if (o->head)
        o->head->prev = l->tail;

    if (l->tail)
        l->tail->next = o->head;
    else
        l->head = o->head;

    if (o->tail) l->tail = o->tail;
    l->len += o->len;

    /* Setup other as an empty list. */
    o->head = o->tail = NULL;
    o->len = 0;
}
