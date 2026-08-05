#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

using namespace std;

// ===================== B+ Tree on (index, value) composite key =====================
// index: up to 64-byte string (no whitespace), stored as 64 bytes null-padded.
// value: int32.
// Ordering: (index, value) lexicographic. find(index) = range scan.
// Persistence: serialize tree to file on exit; load on startup if file exists.
// This is an in-memory B+ tree that persists to a file (file storage BPT).

static const int KEYLEN = 64;
static const int ORDER = 128; // max keys per node

struct CKey {
    char idx[KEYLEN];
    int32_t val;
};

static inline bool cless(const CKey &a, const CKey &b) {
    int c = memcmp(a.idx, b.idx, KEYLEN);
    return c != 0 ? c < 0 : a.val < b.val;
}
static inline bool ceq(const CKey &a, const CKey &b) {
    return memcmp(a.idx, b.idx, KEYLEN) == 0 && a.val == b.val;
}

// Node layout: keys[ORDER], children/next
struct Node {
    bool isLeaf;
    int count; // leaf: #entries; internal: #keys
    CKey keys[ORDER + 1]; // +1 for insertion overflow
    Node *child[ORDER + 2]; // internal: child pointers [0..count]; leaf: child[0]=next
    Node *parent;
};

struct BPlusTree {
    Node *root;

    BPlusTree() : root(nullptr) {}

    Node *mkLeaf() {
        Node *n = new Node();
        n->isLeaf = true; n->count = 0; n->parent = nullptr;
        n->child[0] = nullptr;
        return n;
    }
    Node *mkInternal() {
        Node *n = new Node();
        n->isLeaf = false; n->count = 0; n->parent = nullptr;
        for (int i = 0; i <= ORDER + 1; i++) n->child[i] = nullptr;
        return n;
    }

    void init() { root = mkLeaf(); }

    Node *findLeaf(const CKey &k) {
        Node *cur = root;
        if (!cur) return nullptr;
        while (!cur->isLeaf) {
            int i = 0;
            while (i < cur->count && !cless(k, cur->keys[i])) i++;
            cur = cur->child[i];
        }
        return cur;
    }

    int leafPos(Node *leaf, const CKey &k) {
        int lo = 0, hi = leaf->count;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (cless(leaf->keys[mid], k)) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    void insert(const CKey &k) {
        if (!root) init();
        Node *leaf = findLeaf(k);
        int pos = leafPos(leaf, k);
        if (pos < leaf->count && ceq(leaf->keys[pos], k)) return; // dup
        if (leaf->count < ORDER) {
            for (int i = leaf->count; i > pos; i--) leaf->keys[i] = leaf->keys[i - 1];
            leaf->keys[pos] = k;
            leaf->count++;
        } else {
            CKey tmp[ORDER + 1];
            for (int i = 0; i < pos; i++) tmp[i] = leaf->keys[i];
            tmp[pos] = k;
            for (int i = pos; i < ORDER; i++) tmp[i + 1] = leaf->keys[i];
            int total = ORDER + 1;
            int mid = total / 2;
            Node *right = mkLeaf();
            leaf->count = mid;
            for (int i = 0; i < mid; i++) leaf->keys[i] = tmp[i];
            right->count = total - mid;
            for (int i = 0; i < right->count; i++) right->keys[i] = tmp[mid + i];
            right->child[0] = leaf->child[0];
            leaf->child[0] = right;
            insertInParent(leaf, right->keys[0], right);
        }
    }

    void insertInParent(Node *left, const CKey &key, Node *right) {
        if (left == root) {
            Node *nr = mkInternal();
            nr->count = 1;
            nr->keys[0] = key;
            nr->child[0] = left;
            nr->child[1] = right;
            left->parent = nr;
            right->parent = nr;
            root = nr;
            return;
        }
        Node *parent = left->parent;
        int idx = 0;
        while (idx <= parent->count && parent->child[idx] != left) idx++;
        if (parent->count < ORDER) {
            for (int i = parent->count; i > idx; i--) parent->keys[i] = parent->keys[i - 1];
            for (int i = parent->count + 1; i > idx + 1; i--) parent->child[i] = parent->child[i - 1];
            parent->keys[idx] = key;
            parent->child[idx + 1] = right;
            parent->count++;
            right->parent = parent;
        } else {
            CKey tk[ORDER + 1];
            Node *tc[ORDER + 2];
            for (int i = 0; i < idx; i++) tk[i] = parent->keys[i];
            tk[idx] = key;
            for (int i = idx; i < ORDER; i++) tk[i + 1] = parent->keys[i];
            for (int i = 0; i <= idx; i++) tc[i] = parent->child[i];
            tc[idx + 1] = right;
            for (int i = idx + 1; i <= ORDER; i++) tc[i + 1] = parent->child[i];
            int total = ORDER + 1;
            int mid = total / 2;
            CKey upKey = tk[mid];
            Node *nr = mkInternal();
            parent->count = mid;
            for (int i = 0; i < mid; i++) parent->keys[i] = tk[i];
            for (int i = 0; i <= mid; i++) parent->child[i] = tc[i];
            nr->count = total - mid - 1;
            for (int i = 0; i < nr->count; i++) nr->keys[i] = tk[mid + 1 + i];
            for (int i = 0; i <= nr->count; i++) nr->child[i] = tc[mid + 1 + i];
            for (int i = 0; i <= parent->count; i++) parent->child[i]->parent = parent;
            for (int i = 0; i <= nr->count; i++) nr->child[i]->parent = nr;
            insertInParent(parent, upKey, nr);
        }
    }

    void remove(const CKey &k) {
        if (!root) return;
        Node *leaf = findLeaf(k);
        if (!leaf) return;
        int pos = leafPos(leaf, k);
        if (pos >= leaf->count || !ceq(leaf->keys[pos], k)) return;
        for (int i = pos; i < leaf->count - 1; i++) leaf->keys[i] = leaf->keys[i + 1];
        leaf->count--;
        if (leaf == root || leaf->count >= (ORDER + 1) / 2) return;
        underflow(leaf);
    }

    void underflow(Node *node) {
        if (node == root) {
            if (!node->isLeaf && node->count == 0) {
                Node *nr = node->child[0];
                nr->parent = nullptr;
                root = nr;
                delete node;
            }
            return;
        }
        int minK = (ORDER + 1) / 2;
        if (node->count >= minK) return;

        Node *parent = node->parent;
        int idx = 0;
        while (idx <= parent->count && parent->child[idx] != node) idx++;
        Node *ls = idx > 0 ? parent->child[idx - 1] : nullptr;
        Node *rs = idx < parent->count ? parent->child[idx + 1] : nullptr;

        if (node->isLeaf) {
            if (ls && ls->count > minK) {
                for (int i = node->count; i > 0; i--) node->keys[i] = node->keys[i - 1];
                node->keys[0] = ls->keys[ls->count - 1];
                node->count++; ls->count--;
                parent->keys[idx - 1] = node->keys[0];
                return;
            }
            if (rs && rs->count > minK) {
                node->keys[node->count] = rs->keys[0];
                node->count++;
                for (int i = 0; i < rs->count - 1; i++) rs->keys[i] = rs->keys[i + 1];
                rs->count--;
                parent->keys[idx] = rs->keys[0];
                return;
            }
            if (ls) {
                for (int i = 0; i < node->count; i++) ls->keys[ls->count + i] = node->keys[i];
                ls->count += node->count;
                ls->child[0] = node->child[0];
                rmParentKey(parent, idx - 1, idx);
                delete node;
            } else {
                for (int i = 0; i < rs->count; i++) node->keys[node->count + i] = rs->keys[i];
                node->count += rs->count;
                node->child[0] = rs->child[0];
                rmParentKey(parent, idx, idx + 1);
                delete rs;
            }
        } else {
            if (ls && ls->count > minK) {
                for (int i = node->count; i > 0; i--) node->keys[i] = node->keys[i - 1];
                for (int i = node->count + 1; i > 0; i--) node->child[i] = node->child[i - 1];
                node->keys[0] = parent->keys[idx - 1];
                node->child[0] = ls->child[ls->count];
                node->child[0]->parent = node;
                node->count++;
                parent->keys[idx - 1] = ls->keys[ls->count - 1];
                ls->count--;
                return;
            }
            if (rs && rs->count > minK) {
                node->keys[node->count] = parent->keys[idx];
                node->child[node->count + 1] = rs->child[0];
                node->child[node->count + 1]->parent = node;
                node->count++;
                parent->keys[idx] = rs->keys[0];
                for (int i = 0; i < rs->count - 1; i++) rs->keys[i] = rs->keys[i + 1];
                for (int i = 0; i < rs->count; i++) rs->child[i] = rs->child[i + 1];
                rs->count--;
                return;
            }
            if (ls) {
                ls->keys[ls->count] = parent->keys[idx - 1];
                ls->count++;
                for (int i = 0; i < node->count; i++) {
                    ls->keys[ls->count + i] = node->keys[i];
                    ls->child[ls->count + i] = node->child[i];
                    node->child[i]->parent = ls;
                }
                ls->count += node->count;
                ls->child[ls->count] = node->child[node->count];
                node->child[node->count]->parent = ls;
                rmParentKey(parent, idx - 1, idx);
                delete node;
            } else {
                node->keys[node->count] = parent->keys[idx];
                node->count++;
                for (int i = 0; i < rs->count; i++) {
                    node->keys[node->count + i] = rs->keys[i];
                    node->child[node->count + i] = rs->child[i];
                    rs->child[i]->parent = node;
                }
                node->count += rs->count;
                node->child[node->count] = rs->child[rs->count];
                rs->child[rs->count]->parent = node;
                rmParentKey(parent, idx, idx + 1);
                delete rs;
            }
        }
    }

    void rmParentKey(Node *parent, int ki, int ci) {
        for (int i = ki; i < parent->count - 1; i++) parent->keys[i] = parent->keys[i + 1];
        for (int i = ci; i < parent->count; i++) parent->child[i] = parent->child[i + 1];
        parent->count--;
        underflow(parent);
    }

    // find: output all values for index, ascending, space-separated; "null" if none
    void find(const char *idx, string &out) {
        if (!root) { out += "null\n"; return; }
        CKey lo;
        memset(&lo, 0, sizeof(lo));
        memcpy(lo.idx, idx, strlen(idx));
        lo.val = INT32_MIN;
        Node *leaf = findLeaf(lo);
        if (!leaf) { out += "null\n"; return; }
        int pos = leafPos(leaf, lo);
        bool first = true;
        char buf[KEYLEN];
        memset(buf, 0, KEYLEN);
        memcpy(buf, idx, strlen(idx));
        Node *cur = leaf;
        int cp = pos;
        char numbuf[16];
        while (cur) {
            for (int i = cp; i < cur->count; i++) {
                if (memcmp(cur->keys[i].idx, buf, KEYLEN) != 0) {
                    if (first) out += "null\n";
                    else out += "\n";
                    return;
                }
                if (!first) out += ' ';
                int len = sprintf(numbuf, "%d", cur->keys[i].val);
                out.append(numbuf, len);
                first = false;
            }
            cur = cur->child[0]; // next leaf
            cp = 0;
        }
        if (first) out += "null\n";
        else out += "\n";
    }

    // ===== Persistence =====
    void save(const char *fn) {
        FILE *f = fopen(fn, "wb");
        if (!f) return;
        vector<Node*> nodes;
        dfsCollect(root, nodes);
        int64_t numNodes = (int64_t)nodes.size();
        fwrite(&numNodes, sizeof(int64_t), 1, f);
        int64_t rootId = -1;
        for (int i = 0; i < (int)nodes.size(); i++)
            if (nodes[i] == root) { rootId = i; break; }
        fwrite(&rootId, sizeof(int64_t), 1, f);
        for (int i = 0; i < (int)nodes.size(); i++) {
            Node *n = nodes[i];
            uint8_t lf = n->isLeaf ? 1 : 0;
            fwrite(&lf, 1, 1, f);
            int32_t cnt = n->count;
            fwrite(&cnt, sizeof(int32_t), 1, f);
            for (int j = 0; j < n->count; j++)
                fwrite(&n->keys[j], sizeof(CKey), 1, f);
        }
        for (int i = 0; i < (int)nodes.size(); i++) {
            Node *n = nodes[i];
            if (n->isLeaf) {
                Node *nx = n->child[0];
                int64_t nid = -1;
                for (int k = 0; k < (int)nodes.size(); k++)
                    if (nx && nodes[k] == nx) { nid = k; break; }
                fwrite(&nid, sizeof(int64_t), 1, f);
            } else {
                for (int j = 0; j <= n->count; j++) {
                    int64_t cid = -1;
                    for (int k = 0; k < (int)nodes.size(); k++)
                        if (nodes[k] == n->child[j]) { cid = k; break; }
                    fwrite(&cid, sizeof(int64_t), 1, f);
                }
            }
        }
        fclose(f);
    }

    void dfsCollect(Node *n, vector<Node*> &nodes) {
        if (!n) return;
        nodes.push_back(n);
        if (!n->isLeaf)
            for (int i = 0; i <= n->count; i++) dfsCollect(n->child[i], nodes);
    }

    void load(const char *fn) {
        FILE *f = fopen(fn, "rb");
        if (!f) return;
        int64_t numNodes, rootId;
        if (fread(&numNodes, sizeof(int64_t), 1, f) != 1) { fclose(f); return; }
        if (fread(&rootId, sizeof(int64_t), 1, f) != 1) { fclose(f); return; }
        if (numNodes <= 0 || rootId < 0) { fclose(f); return; }
        vector<Node*> nodes(numNodes, nullptr);
        for (int i = 0; i < numNodes; i++) {
            uint8_t lf;
            if (fread(&lf, 1, 1, f) != 1) break;
            int32_t cnt;
            if (fread(&cnt, sizeof(int32_t), 1, f) != 1) break;
            Node *n = lf ? mkLeaf() : mkInternal();
            n->count = cnt;
            for (int j = 0; j < cnt; j++)
                if (fread(&n->keys[j], sizeof(CKey), 1, f) != 1) break;
            nodes[i] = n;
        }
        for (int i = 0; i < numNodes; i++) {
            Node *n = nodes[i];
            if (!n) continue;
            if (n->isLeaf) {
                int64_t nid;
                if (fread(&nid, sizeof(int64_t), 1, f) != 1) break;
                n->child[0] = (nid >= 0 && nid < numNodes) ? nodes[nid] : nullptr;
            } else {
                for (int j = 0; j <= n->count; j++) {
                    int64_t cid;
                    if (fread(&cid, sizeof(int64_t), 1, f) != 1) break;
                    if (cid >= 0 && cid < numNodes) {
                        n->child[j] = nodes[cid];
                        nodes[cid]->parent = n;
                    }
                }
            }
        }
        fclose(f);
        root = (rootId >= 0 && rootId < numNodes) ? nodes[rootId] : nullptr;
    }
};

int main() {
    // Fast I/O
    BPlusTree tree;
    const char *DATAFILE = "bpt_data.bin";

    // Check if file exists and has content
    bool hasFile = false;
    {
        FILE *chk = fopen(DATAFILE, "rb");
        if (chk) {
            fseek(chk, 0, SEEK_END);
            long sz = ftell(chk);
            fclose(chk);
            if (sz > 0) hasFile = true;
        }
    }
    if (hasFile) {
        tree.load(DATAFILE);
    }
    if (!tree.root) tree.init();

    // Read input
    // Use fast reading
    string out;
    out.reserve(1 << 20);

    int n;
    if (scanf("%d", &n) != 1) return 0;

    char cmd[16];
    char idx[KEYLEN + 1];
    int val;

    for (int i = 0; i < n; i++) {
        if (scanf("%15s", cmd) != 1) break;
        if (cmd[0] == 'i') {
            // insert
            scanf("%64s %d", idx, &val);
            CKey k;
            memset(&k, 0, sizeof(k));
            memcpy(k.idx, idx, strlen(idx));
            k.val = val;
            tree.insert(k);
        } else if (cmd[0] == 'd') {
            // delete
            scanf("%64s %d", idx, &val);
            CKey k;
            memset(&k, 0, sizeof(k));
            memcpy(k.idx, idx, strlen(idx));
            k.val = val;
            tree.remove(k);
        } else {
            // find
            scanf("%64s", idx);
            tree.find(idx, out);
        }
    }

    fputs(out.c_str(), stdout);
    fflush(stdout);

    tree.save(DATAFILE);
    return 0;
}
