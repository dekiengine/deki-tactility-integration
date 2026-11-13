// The tree erase a Tactility app defines for itself (TactilityTreeErase.h),
// run against the host's libstdc++ insert: whatever is inserted and erased,
// in any order, the tree stays a valid red-black tree with the right nodes
// in order and the header pointing at its ends.

#include "TactilityTreeErase.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

namespace
{

using Base = std::_Rb_tree_node_base;

struct IntNode : Base
{
    int value = 0;
};

struct Tree
{
    Base header{};
    std::vector<std::unique_ptr<IntNode>> nodes;
    size_t count = 0;

    Tree()
    {
        header._M_color = std::_S_red;
        header._M_parent = nullptr;
        header._M_left = &header;
        header._M_right = &header;
    }

    static int ValueOf(const Base* node) { return static_cast<const IntNode*>(node)->value; }

    IntNode* Insert(int value)
    {
        auto node = std::make_unique<IntNode>();
        node->value = value;
        Base* parent = &header;
        Base* at = header._M_parent;
        bool left = true;
        while (at)
        {
            parent = at;
            left = value < ValueOf(at);
            at = left ? at->_M_left : at->_M_right;
        }
        std::_Rb_tree_insert_and_rebalance(left, node.get(), parent, header);
        ++count;
        nodes.push_back(std::move(node));
        return nodes.back().get();
    }

    void Erase(IntNode* node)
    {
        Base* out = DekiTactility::RebalanceForErase(node, header);
        ASSERT_EQ(out, node);
        --count;
        nodes.erase(std::find_if(nodes.begin(), nodes.end(), [&](const auto& n) { return n.get() == node; }));
    }

    // The black height below a node, or -1 when the tree is broken there.
    static int BlackHeight(const Base* node, const Base* parent)
    {
        if (!node)
        {
            return 1;
        }
        if (node->_M_parent != parent)
        {
            return -1;
        }
        if (node->_M_color == std::_S_red && ((node->_M_left && node->_M_left->_M_color == std::_S_red) ||
                                              (node->_M_right && node->_M_right->_M_color == std::_S_red)))
        {
            return -1;
        }
        if ((node->_M_left && ValueOf(node->_M_left) > ValueOf(node)) ||
            (node->_M_right && ValueOf(node->_M_right) < ValueOf(node)))
        {
            return -1;
        }
        const int left = BlackHeight(node->_M_left, node);
        const int right = BlackHeight(node->_M_right, node);
        if (left < 0 || left != right)
        {
            return -1;
        }
        return left + (node->_M_color == std::_S_black ? 1 : 0);
    }

    // Every red-black rule, the parent links, the order, and the header.
    bool Valid(const std::vector<int>& expected) const
    {
        const Base* root = header._M_parent;
        if (!root)
        {
            return expected.empty() && header._M_left == &header && header._M_right == &header;
        }
        if (root->_M_color != std::_S_black || BlackHeight(root, &header) < 0)
        {
            return false;
        }
        if (header._M_left != Base::_S_minimum(const_cast<Base*>(root)) ||
            header._M_right != Base::_S_maximum(const_cast<Base*>(root)))
        {
            return false;
        }
        std::vector<int> inOrder;
        for (Base* at = header._M_left; at != &header; at = std::_Rb_tree_increment(at))
        {
            inOrder.push_back(ValueOf(at));
        }
        return inOrder == expected;
    }
};

}  // namespace

TEST(TreeErase, TheOnlyNodeLeavesAnEmptyTree)
{
    Tree tree;
    IntNode* node = tree.Insert(7);
    tree.Erase(node);
    EXPECT_TRUE(tree.Valid({}));
}

TEST(TreeErase, EveryNodeOfASmallTreeCanGoFirst)
{
    for (int first = 0; first < 9; ++first)
    {
        Tree tree;
        std::vector<IntNode*> nodes;
        for (int v = 0; v < 9; ++v)
        {
            nodes.push_back(tree.Insert(v));
        }
        tree.Erase(nodes[first]);
        std::vector<int> expected;
        for (int v = 0; v < 9; ++v)
        {
            if (v != first)
            {
                expected.push_back(v);
            }
        }
        EXPECT_TRUE(tree.Valid(expected)) << "erasing " << first;
    }
}

TEST(TreeErase, RandomInsertsAndErasesKeepAValidTree)
{
    std::mt19937 random(1234);
    for (int round = 0; round < 50; ++round)
    {
        Tree tree;
        std::vector<int> expected;
        std::vector<IntNode*> live;
        for (int step = 0; step < 400; ++step)
        {
            if (live.empty() || random() % 3 != 0)
            {
                const int value = static_cast<int>(random() % 1000);
                live.push_back(tree.Insert(value));
                expected.insert(std::upper_bound(expected.begin(), expected.end(), value), value);
            }
            else
            {
                const size_t at = random() % live.size();
                IntNode* node = live[at];
                const int value = node->value;
                live.erase(live.begin() + static_cast<long>(at));
                tree.Erase(node);
                expected.erase(std::find(expected.begin(), expected.end(), value));
            }
            ASSERT_TRUE(tree.Valid(expected)) << "round " << round << " step " << step;
        }
        while (!live.empty())
        {
            IntNode* node = live.back();
            live.pop_back();
            expected.erase(std::find(expected.begin(), expected.end(), node->value));
            tree.Erase(node);
            ASSERT_TRUE(tree.Valid(expected));
        }
    }
}
