#pragma once

// Taking a node out of a std::map or std::set's red-black tree, for Tactility,
// whose firmware exports libstdc++'s insert (std::_Rb_tree_insert_and_rebalance)
// to apps but not its erase (std::_Rb_tree_rebalance_for_erase).
// TactilityLibcxx.cpp defines that erase with this.
//
// It works on libstdc++'s own node and header (header.parent is the root,
// header.left the leftmost node, header.right the rightmost), so it and the
// firmware's insert can work on the same tree. It is the textbook delete:
// a node with two children trades places with the next node, as nodes, since
// the values cannot be moved; then the colours are mended going up.

#include <bits/stl_tree.h>

#include <utility>

namespace DekiTactility
{

namespace TreeErase
{

using Node = std::_Rb_tree_node_base;

inline bool IsBlack(const Node* node)
{
    return node == nullptr || node->_M_color == std::_S_black;
}

inline void RotateLeft(Node* x, Node*& root)
{
    Node* y = x->_M_right;
    x->_M_right = y->_M_left;
    if (y->_M_left)
    {
        y->_M_left->_M_parent = x;
    }
    y->_M_parent = x->_M_parent;
    if (x == root)
    {
        root = y;
    }
    else if (x == x->_M_parent->_M_left)
    {
        x->_M_parent->_M_left = y;
    }
    else
    {
        x->_M_parent->_M_right = y;
    }
    y->_M_left = x;
    x->_M_parent = y;
}

inline void RotateRight(Node* x, Node*& root)
{
    Node* y = x->_M_left;
    x->_M_left = y->_M_right;
    if (y->_M_right)
    {
        y->_M_right->_M_parent = x;
    }
    y->_M_parent = x->_M_parent;
    if (x == root)
    {
        root = y;
    }
    else if (x == x->_M_parent->_M_right)
    {
        x->_M_parent->_M_right = y;
    }
    else
    {
        x->_M_parent->_M_left = y;
    }
    y->_M_right = x;
    x->_M_parent = y;
}

}  // namespace TreeErase

// Unlinks z from the tree whose header is given and keeps it balanced.
// Returns z, which the caller then frees.
inline std::_Rb_tree_node_base* RebalanceForErase(std::_Rb_tree_node_base* const z,
                                                  std::_Rb_tree_node_base& header) noexcept
{
    using namespace TreeErase;
    Node*& root = header._M_parent;
    Node*& leftmost = header._M_left;
    Node*& rightmost = header._M_right;

    // y is the node that leaves its place: z, or the next node after z when z
    // has two children. x takes y's place, and may be null.
    Node* y = z;
    Node* x = nullptr;
    Node* xParent = nullptr;
    if (y->_M_left == nullptr)
    {
        x = y->_M_right;
    }
    else if (y->_M_right == nullptr)
    {
        x = y->_M_left;
    }
    else
    {
        y = y->_M_right;
        while (y->_M_left)
        {
            y = y->_M_left;
        }
        x = y->_M_right;
    }

    if (y != z)
    {
        // The next node takes z's place and z's colour.
        z->_M_left->_M_parent = y;
        y->_M_left = z->_M_left;
        if (y != z->_M_right)
        {
            xParent = y->_M_parent;
            if (x)
            {
                x->_M_parent = y->_M_parent;
            }
            y->_M_parent->_M_left = x;
            y->_M_right = z->_M_right;
            z->_M_right->_M_parent = y;
        }
        else
        {
            xParent = y;
        }
        if (root == z)
        {
            root = y;
        }
        else if (z->_M_parent->_M_left == z)
        {
            z->_M_parent->_M_left = y;
        }
        else
        {
            z->_M_parent->_M_right = y;
        }
        y->_M_parent = z->_M_parent;
        std::swap(y->_M_color, z->_M_color);
        y = z;  // z's old colour is what left the tree
    }
    else
    {
        xParent = y->_M_parent;
        if (x)
        {
            x->_M_parent = y->_M_parent;
        }
        if (root == z)
        {
            root = x;
        }
        else if (z->_M_parent->_M_left == z)
        {
            z->_M_parent->_M_left = x;
        }
        else
        {
            z->_M_parent->_M_right = x;
        }
        // z had at most one child, so the ends move to it or to z's parent.
        if (leftmost == z)
        {
            leftmost = z->_M_right == nullptr ? z->_M_parent : Node::_S_minimum(x);
        }
        if (rightmost == z)
        {
            rightmost = z->_M_left == nullptr ? z->_M_parent : Node::_S_maximum(x);
        }
    }

    if (y->_M_color != std::_S_red)
    {
        // A black node left: x's side is one black short. Mend it going up.
        while (x != root && IsBlack(x))
        {
            if (x == xParent->_M_left)
            {
                Node* w = xParent->_M_right;
                if (w->_M_color == std::_S_red)
                {
                    w->_M_color = std::_S_black;
                    xParent->_M_color = std::_S_red;
                    RotateLeft(xParent, root);
                    w = xParent->_M_right;
                }
                if (IsBlack(w->_M_left) && IsBlack(w->_M_right))
                {
                    w->_M_color = std::_S_red;
                    x = xParent;
                    xParent = xParent->_M_parent;
                    continue;
                }
                if (IsBlack(w->_M_right))
                {
                    w->_M_left->_M_color = std::_S_black;
                    w->_M_color = std::_S_red;
                    RotateRight(w, root);
                    w = xParent->_M_right;
                }
                w->_M_color = xParent->_M_color;
                xParent->_M_color = std::_S_black;
                if (w->_M_right)
                {
                    w->_M_right->_M_color = std::_S_black;
                }
                RotateLeft(xParent, root);
                break;
            }

            Node* w = xParent->_M_left;
            if (w->_M_color == std::_S_red)
            {
                w->_M_color = std::_S_black;
                xParent->_M_color = std::_S_red;
                RotateRight(xParent, root);
                w = xParent->_M_left;
            }
            if (IsBlack(w->_M_right) && IsBlack(w->_M_left))
            {
                w->_M_color = std::_S_red;
                x = xParent;
                xParent = xParent->_M_parent;
                continue;
            }
            if (IsBlack(w->_M_left))
            {
                w->_M_right->_M_color = std::_S_black;
                w->_M_color = std::_S_red;
                RotateLeft(w, root);
                w = xParent->_M_left;
            }
            w->_M_color = xParent->_M_color;
            xParent->_M_color = std::_S_black;
            if (w->_M_left)
            {
                w->_M_left->_M_color = std::_S_black;
            }
            RotateRight(xParent, root);
            break;
        }
        if (x)
        {
            x->_M_color = std::_S_black;
        }
    }
    return y;
}

}  // namespace DekiTactility
