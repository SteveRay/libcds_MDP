//
//  LockFreeLinkedList.h
//  Lock-Free Ordered Linked List
//
//  Created by Станислав Райцин on 03.02.15.
//  Copyright (c) 2015 Станислав Райцин. All rights reserved.
//

#ifndef __Lock_Free_Ordered_Linked_List__LockFreeLinkedList__
#define __Lock_Free_Ordered_Linked_List__LockFreeLinkedList__

#include <atomic>
#include "NodeTypes.h"
#include "LFLLIterator.h"

template <typename T>
class LockFreeLinkedList {
public:
    LockFreeLinkedList(unsigned int capacity = 100) :
    list_capacity(capacity),
    FIRST_NODE(new CellNode<T>(NULL)),
    LAST_NODE(new CellNode<T>(NULL)),
    freeCellsList(new CellNode<T>()),
    freeAuxNodesList(new Node<T>()) { initialize(); }
    
    void            _insert(T key);
    void            _delete(T key);
    bool            _find(T key);
    
private:
    void            _update(LFLLIterator<T>* it);
    void            _first(LFLLIterator<T>* it);
    bool            _next(LFLLIterator<T>* it);
    bool            _tryInsert(LFLLIterator<T>* it, CellNode<T>* q, Node<T>* a);
    bool            _tryDelete(LFLLIterator<T>* it);
    bool            _findFrom(T key, LFLLIterator<T>* it);
    Node<T>*        _safeRead(std::atomic<Node<T>*>* p);
    void            _release(Node<T>* p);
    bool            _decrementAndTestAndSet(std::atomic_long* p);
    void            _clearLowestBit(std::atomic_long* p);
    void            _reclaim(Node<T>* p);
    Node<T>*        _allocAuxNode();
    CellNode<T>*    _allocCellNode();                        
    
    void    initialize();
    
    unsigned int                    list_capacity;
    std::atomic<Node<T>*>           FIRST_NODE;
    std::atomic<Node<T>*>           LAST_NODE;
    std::atomic<Node<T>*>           freeCellsList;
    std::atomic<Node<T>*>           freeAuxNodesList;
};


template <typename T>
void LockFreeLinkedList<T>::_insert(T key) {
    LFLLIterator<T>* it = new LFLLIterator<T>();
    _first(it);
    CellNode<T>*     q = _allocCellNode();
    Node<T>*         a = _allocAuxNode();
    q->data = key;
    while (1) {
        bool r = _findFrom(key, it);
        if (r) { break; }
        r = _tryInsert(it, q, a);
        if (r) { break; }
        _update(it);
    }
    _release(it->pre_cell.load());
    _release(it->pre_aux.load());
    _release(it->target.load());
}

template <typename T>
void LockFreeLinkedList<T>::_delete(T key) {
    LFLLIterator<T>* it = new LFLLIterator<T>();
    _first(it);
    while (1) {
        bool r = _findFrom(key, it);
        if (!r) { break; }
        r = _tryDelete(it);
        if (r) { break; }
        _update(it);
    }
    _release(it->pre_cell.load());
    _release(it->pre_aux.load());
    _release(it->target.load());
}

template <typename T>
bool LockFreeLinkedList<T>::_find(T key) {
    LFLLIterator<T>* it = new LFLLIterator<T>();
    _first(it);
    bool r = _findFrom(key, it);
    _release(it->pre_cell.load());
    _release(it->pre_aux.load());
    _release(it->target.load());
    return r;
}


template <typename T>
void LockFreeLinkedList<T>::_update(LFLLIterator<T>* it) {
    Node<T>*        p = it->pre_aux.load();
    if (p->next.load() == it->target.load()) {
        return;
    }
    Node<T>*        n = _safeRead(&p->next);
    _release(it->target.load());
    while (n != LAST_NODE.load() && !dynamic_cast<CellNode<T>*>(n)) {
        it->pre_cell.load()->next.compare_exchange_weak(p, n);
        _release(p);
        p = n;
        n = _safeRead(&p->next);
    }
    it->pre_aux.store(p);
    it->target.store(dynamic_cast<CellNode<T>*>(n));
}

template <typename T>
void LockFreeLinkedList<T>::_first(LFLLIterator<T>* it) {
    it->pre_cell.store(dynamic_cast<CellNode<T>*>(_safeRead(&FIRST_NODE)));
    it->pre_aux.store(_safeRead(&FIRST_NODE.load()->next));
    it->target.store(nullptr);
    _update(it);
}

template <typename T>
bool LockFreeLinkedList<T>::_next(LFLLIterator<T>* it) {
    if (it->target.load() == LAST_NODE.load()) {
        return false;
    } else {
        _release(it->pre_cell.load());
        it->pre_cell.store(dynamic_cast<CellNode<T>*>(_safeRead(&it->target)));
        _release(it->pre_aux.load());
        it->pre_aux.store(_safeRead(&it->target.load()->next));
        _update(it);
        return true;
    }
}

template <typename T>
bool LockFreeLinkedList<T>::_tryInsert(LFLLIterator<T>* it, CellNode<T>* q, Node<T>* a) {
    q->next.store(a);
    a->next.store(it->target.load());
    Node<T>* p = it->target.load();
    bool r = it->pre_aux.load()->next.compare_exchange_weak(p, q);
    return r;
}

template <typename T>
bool LockFreeLinkedList<T>::_tryDelete(LFLLIterator<T>* it) {
    Node<T>*         dN = it->target.load();
    Node<T>*         n = it->target.load()->next.load();
    bool r = it->pre_aux.load()->next.compare_exchange_weak(dN, n);
    if (!r) {
        return false;
    } else {
        CellNode<T>* d = dynamic_cast<CellNode<T>*>(dN);
        d->back_link.store(it->pre_cell.load());
        _clearLowestBit(&d->refct_claim);
        CellNode<T>* p = dynamic_cast<CellNode<T>*>(it->pre_cell.load());
        while (p->back_link.load() != nullptr) {
            CellNode<T>* q = dynamic_cast<CellNode<T>*>(_safeRead(&p->back_link));
            _release(p);
            p = q;
        }
        Node<T>*     s = _safeRead(&p->next);
        while (!dynamic_cast<CellNode<T>*>(n->next.load())) {
            Node<T>* q = _safeRead(&n->next);
            _release(n);
            n = q;
        }
        do {
            r = p->next.compare_exchange_weak(s, n);
            if (!r) {
                _release(s);
                s = _safeRead(&p->next);
            }
        } while (!r && p->back_link.load() == nullptr
                 && dynamic_cast<CellNode<T>*>(n->next.load()));
        _release(p);
        _clearLowestBit(&s->refct_claim);
        _release(s);
        _release(n);
        return true;
    }
}

template <typename T>
bool LockFreeLinkedList<T>::_findFrom(T key, LFLLIterator<T> *it) {
    CellNode<T>* n = dynamic_cast<CellNode<T>*>(it->target.load());
    while (it->target.load() != LAST_NODE.load()) {
        if (n != nullptr) {
            if (n->data == key) {
                return true;
            }
        }
        _next(it);
    }
    return false;
}

template <typename T>
Node<T>* LockFreeLinkedList<T>::_safeRead(std::atomic<Node<T>*>* p) {
    while (1) {
        Node<T>* q = p->load();
        if (q == nullptr) {
            return nullptr;
        }
        q->refct_claim.fetch_add(2);
        if (q == p->load()) {
            return q;
        } else {
            _release(q);
        }
    }
}

template <typename T>
void LockFreeLinkedList<T>::_release(Node<T> *p) {
    if (p == nullptr) {
        return;
    }
    if (!_decrementAndTestAndSet(&p->refct_claim)) {
        return;
    }
    _reclaim(p);
}

template <typename T>
bool LockFreeLinkedList<T>::_decrementAndTestAndSet(std::atomic_long* p) {
    long oldVal, newVal;
    do {
        oldVal = p->load();
        newVal = oldVal - 2;
        if (newVal <= 0) {
            newVal = 1;
        }
    } while (!p->compare_exchange_weak(oldVal, newVal));
    return (oldVal - newVal) & 1;
}

template <typename T>
void LockFreeLinkedList<T>::_clearLowestBit(std::atomic_long* p) {
    long oldVal, newVal;
    do {
        oldVal = p->load();
        newVal = oldVal - 1;
    } while (!p->compare_exchange_weak(oldVal, newVal));
}


template <typename T>
void LockFreeLinkedList<T>::_reclaim(Node<T>* p) {
    Node<T>* q = nullptr;
    if (dynamic_cast<CellNode<T>*>(p) != nullptr) {
        do {
            q = freeCellsList.load();
            p->next.store(q);
        } while (!freeCellsList.compare_exchange_weak(q, p));
    } else {
        do {
            q = freeAuxNodesList.load();
            p->next.store(q);
        } while (!freeAuxNodesList.compare_exchange_weak(q, p));
    }
    _clearLowestBit(&p->refct_claim);
}

template <typename T>
Node<T>* LockFreeLinkedList<T>::_allocAuxNode() {
    while (1) {
        Node<T>* p = _safeRead(&freeAuxNodesList);
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        if (freeAuxNodesList.compare_exchange_weak(p, p->next.load())) {
            _clearLowestBit(&p->refct_claim);
            return p;
        } else {
            _release(p);
        }
    }
}

template <typename T>
CellNode<T>* LockFreeLinkedList<T>::_allocCellNode() {
    while (1) {
        Node<T>* p = _safeRead(&freeCellsList);
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        if (freeCellsList.compare_exchange_weak(p, p->next.load())) {
            _clearLowestBit(&p->refct_claim);
            CellNode<T>* cellNode = dynamic_cast<CellNode<T>*>(p);
            p = nullptr;
            cellNode->back_link.store(nullptr);
            return cellNode;
        } else {
            _release(p);
        }
    }
}

template <typename T>
void LockFreeLinkedList<T>::initialize() {
    FIRST_NODE.load()->refct_claim = 1;
    LAST_NODE.load()->refct_claim = 1;
    Node<T>* aux = new Node<T>();
    aux->refct_claim = 1;
    aux->next.store(LAST_NODE.load());
    FIRST_NODE.load()->next.store(aux);
    
    if (list_capacity > 0) {
        Node<T>* auxCurrNode = freeAuxNodesList.load();
        Node<T>* cellCurrNode = freeCellsList.load();
        for (unsigned int i = 1; i < list_capacity; i++) {
            Node<T>* newAuxNode = new Node<T>();
            Node<T>* newCellNode = new CellNode<T>();
            auxCurrNode->next.store(newAuxNode);
            cellCurrNode->next.store(newCellNode);
            auxCurrNode = newAuxNode;
            cellCurrNode = newCellNode;
        }
    }
}
#endif /* defined(__Lock_Free_Ordered_Linked_List__LockFreeLinkedList__) */