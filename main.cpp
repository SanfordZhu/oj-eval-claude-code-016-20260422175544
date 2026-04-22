#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

using namespace std;

const char* DB_FILE = "bpt.db";
const int MAX_KEY_LEN = 64;
const int ORDER = 128;  // Maximum children per node
const int MAX_KEYS = ORDER - 1;  // Maximum keys per node
const int MIN_KEYS = (MAX_KEYS + 1) / 2;  // Minimum keys per node

// File header structure
struct FileHeader {
    int64_t root_offset;
    int64_t next_offset;
};

// Node structure on disk
struct Node {
    bool is_leaf;
    int16_t key_count;
    char keys[MAX_KEYS][MAX_KEY_LEN];
    int64_t children[ORDER];  // For internal nodes: child offsets; For leaf nodes: values
    int64_t next_leaf;         // Only for leaf nodes

    Node() : is_leaf(false), key_count(0), next_leaf(-1) {
        memset(keys, 0, sizeof(keys));
        memset(children, -1, sizeof(children));
    }
};

class BPT {
private:
    fstream db;
    FileHeader header;
    int64_t node_size = sizeof(Node);

    void open_db() {
        db.open(DB_FILE, ios::in | ios::out | ios::binary);
        if (!db.is_open()) {
            db.open(DB_FILE, ios::out | ios::binary);
            db.close();
            db.open(DB_FILE, ios::in | ios::out | ios::binary);
            header.root_offset = -1;
            header.next_offset = sizeof(FileHeader);
            write_header();
        } else {
            read_header();
        }
    }

    void close_db() {
        db.close();
    }

    void read_header() {
        db.seekg(0);
        db.read(reinterpret_cast<char*>(&header), sizeof(FileHeader));
    }

    void write_header() {
        db.seekp(0);
        db.write(reinterpret_cast<char*>(&header), sizeof(FileHeader));
    }

    Node read_node(int64_t offset) {
        Node node;
        db.seekg(offset);
        db.read(reinterpret_cast<char*>(&node), sizeof(Node));
        return node;
    }

    void write_node(int64_t offset, const Node& node) {
        db.seekp(offset);
        db.write(reinterpret_cast<const char*>(&node), sizeof(Node));
    }

    int64_t allocate_node() {
        int64_t offset = header.next_offset;
        header.next_offset += node_size;
        write_header();
        return offset;
    }

    int compare_keys(const char* key1, const char* key2) {
        return strcmp(key1, key2);
    }

    int find_key_index(Node& node, const string& key) {
        int lo = 0, hi = node.key_count;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (compare_keys(node.keys[mid], key.c_str()) < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    bool insert_into_leaf(Node& node, const string& key, int64_t value) {
        int idx = find_key_index(node, key);
        // Check if key already exists
        if (idx < node.key_count && compare_keys(node.keys[idx], key.c_str()) == 0) {
            // Check if value already exists for this key
            for (int i = idx; i < node.key_count && compare_keys(node.keys[i], key.c_str()) == 0; i++) {
                if (node.children[i] == value) {
                    return false;  // Duplicate (key, value) pair
                }
            }
            // Insert value at correct position (maintain sorted order)
            while (idx < node.key_count && compare_keys(node.keys[idx], key.c_str()) == 0 &&
                   node.children[idx] < value) {
                idx++;
            }
        }
        // Shift keys and values to make room
        for (int i = node.key_count; i > idx; i--) {
            strcpy(node.keys[i], node.keys[i - 1]);
            node.children[i] = node.children[i - 1];
        }
        strcpy(node.keys[idx], key.c_str());
        node.children[idx] = value;
        node.key_count++;
        return true;
    }

    bool insert_into_internal(Node& node, const string& key, int64_t child_offset) {
        int idx = find_key_index(node, key);
        // Shift keys and children to make room
        for (int i = node.key_count; i > idx; i--) {
            strcpy(node.keys[i], node.keys[i - 1]);
            node.children[i + 1] = node.children[i];
        }
        strcpy(node.keys[idx], key.c_str());
        node.children[idx + 1] = child_offset;
        node.key_count++;
        return true;
    }

    bool remove_from_leaf(Node& node, const string& key, int64_t value) {
        int idx = find_key_index(node, key);
        // Find exact key-value pair
        while (idx < node.key_count && compare_keys(node.keys[idx], key.c_str()) == 0) {
            if (node.children[idx] == value) {
                // Found, remove it
                for (int i = idx; i < node.key_count - 1; i++) {
                    strcpy(node.keys[i], node.keys[i + 1]);
                    node.children[i] = node.children[i + 1];
                }
                node.key_count--;
                return true;
            }
            idx++;
        }
        return false;
    }

    void split_leaf(int64_t offset, Node& node, string& split_key, int64_t& new_offset) {
        Node new_node;
        new_node.is_leaf = true;
        new_node.next_leaf = node.next_leaf;
        node.next_leaf = new_offset = allocate_node();

        int mid = (MAX_KEYS + 1) / 2;
        split_key = string(node.keys[mid]);

        // Copy second half to new node
        for (int i = mid; i < node.key_count; i++) {
            strcpy(new_node.keys[i - mid], node.keys[i]);
            new_node.children[i - mid] = node.children[i];
        }
        new_node.key_count = node.key_count - mid;
        node.key_count = mid;

        write_node(offset, node);
        write_node(new_offset, new_node);
    }

    void split_internal(int64_t offset, Node& node, string& split_key, int64_t& new_offset) {
        Node new_node;
        new_node.is_leaf = false;
        new_offset = allocate_node();

        int mid = MAX_KEYS / 2;
        split_key = string(node.keys[mid]);

        // Copy second half to new node (excluding the middle key which goes up)
        for (int i = mid + 1; i < node.key_count; i++) {
            strcpy(new_node.keys[i - mid - 1], node.keys[i]);
            new_node.children[i - mid - 1] = node.children[i];
        }
        new_node.children[node.key_count - mid - 1] = node.children[node.key_count];
        new_node.key_count = node.key_count - mid - 1;
        node.key_count = mid;

        write_node(offset, node);
        write_node(new_offset, new_node);
    }

    bool insert_recursive(int64_t offset, const string& key, int64_t value,
                          string& split_key, int64_t& split_offset) {
        Node node = read_node(offset);

        if (node.is_leaf) {
            if (node.key_count < MAX_KEYS) {
                if (insert_into_leaf(node, key, value)) {
                    write_node(offset, node);
                    return false;  // No split needed
                }
                return false;  // Duplicate
            }
            // Need to split
            if (!insert_into_leaf(node, key, value)) {
                return false;  // Duplicate
            }
            split_leaf(offset, node, split_key, split_offset);
            return true;
        } else {
            int idx = find_key_index(node, key);
            string child_split_key;
            int64_t child_split_offset;
            bool split = insert_recursive(node.children[idx], key, value, child_split_key, child_split_offset);

            if (!split) return false;

            // Child split, need to insert split key into this node
            if (node.key_count < MAX_KEYS) {
                insert_into_internal(node, child_split_key, child_split_offset);
                write_node(offset, node);
                return false;
            }
            // Need to split this node
            insert_into_internal(node, child_split_key, child_split_offset);
            split_internal(offset, node, split_key, split_offset);
            return true;
        }
    }

    bool delete_recursive(int64_t offset, const string& key, int64_t value, bool& underflow) {
        Node node = read_node(offset);
        underflow = false;

        if (node.is_leaf) {
            if (remove_from_leaf(node, key, value)) {
                write_node(offset, node);
                underflow = (node.key_count < MIN_KEYS);
                return true;
            }
            return false;
        } else {
            int idx = find_key_index(node, key);
            bool child_underflow;
            bool found = delete_recursive(node.children[idx], key, value, child_underflow);

            if (!found) return false;

            if (child_underflow) {
                // Try to borrow from sibling or merge
                Node child = read_node(node.children[idx]);

                // Try to borrow from left sibling
                if (idx > 0) {
                    Node left_sibling = read_node(node.children[idx - 1]);
                    if (left_sibling.key_count > MIN_KEYS) {
                        // Borrow from left
                        if (child.is_leaf) {
                            // Shift child entries right
                            for (int i = child.key_count; i > 0; i--) {
                                strcpy(child.keys[i], child.keys[i - 1]);
                                child.children[i] = child.children[i - 1];
                            }
                            strcpy(child.keys[0], left_sibling.keys[left_sibling.key_count - 1]);
                            child.children[0] = left_sibling.children[left_sibling.key_count - 1];
                            left_sibling.key_count--;
                            child.key_count++;
                            strcpy(node.keys[idx - 1], child.keys[0]);
                        } else {
                            // Internal node
                            for (int i = child.key_count; i > 0; i--) {
                                strcpy(child.keys[i], child.keys[i - 1]);
                            }
                            for (int i = child.key_count + 1; i > 0; i--) {
                                child.children[i] = child.children[i - 1];
                            }
                            strcpy(child.keys[0], node.keys[idx - 1]);
                            child.children[0] = left_sibling.children[left_sibling.key_count];
                            strcpy(node.keys[idx - 1], left_sibling.keys[left_sibling.key_count - 1]);
                            left_sibling.key_count--;
                            child.key_count++;
                        }
                        write_node(node.children[idx], child);
                        write_node(node.children[idx - 1], left_sibling);
                        write_node(offset, node);
                        return true;
                    }
                }

                // Try to borrow from right sibling
                if (idx < node.key_count) {
                    Node right_sibling = read_node(node.children[idx + 1]);
                    if (right_sibling.key_count > MIN_KEYS) {
                        // Borrow from right
                        if (child.is_leaf) {
                            strcpy(child.keys[child.key_count], right_sibling.keys[0]);
                            child.children[child.key_count] = right_sibling.children[0];
                            child.key_count++;
                            // Update parent key
                            if (right_sibling.key_count > 1) {
                                strcpy(node.keys[idx], right_sibling.keys[1]);
                            }
                            // Shift right sibling left
                            for (int i = 0; i < right_sibling.key_count - 1; i++) {
                                strcpy(right_sibling.keys[i], right_sibling.keys[i + 1]);
                                right_sibling.children[i] = right_sibling.children[i + 1];
                            }
                            right_sibling.key_count--;
                        } else {
                            strcpy(child.keys[child.key_count], node.keys[idx]);
                            child.children[child.key_count + 1] = right_sibling.children[0];
                            child.key_count++;
                            strcpy(node.keys[idx], right_sibling.keys[0]);
                            // Shift right sibling left
                            for (int i = 0; i < right_sibling.key_count - 1; i++) {
                                strcpy(right_sibling.keys[i], right_sibling.keys[i + 1]);
                            }
                            for (int i = 0; i < right_sibling.key_count; i++) {
                                right_sibling.children[i] = right_sibling.children[i + 1];
                            }
                            right_sibling.key_count--;
                        }
                        write_node(node.children[idx], child);
                        write_node(node.children[idx + 1], right_sibling);
                        write_node(offset, node);
                        return true;
                    }
                }

                // Merge with sibling
                if (idx > 0) {
                    // Merge with left sibling
                    Node left_sibling = read_node(node.children[idx - 1]);
                    if (child.is_leaf) {
                        for (int i = 0; i < child.key_count; i++) {
                            strcpy(left_sibling.keys[left_sibling.key_count + i], child.keys[i]);
                            left_sibling.children[left_sibling.key_count + i] = child.children[i];
                        }
                        left_sibling.key_count += child.key_count;
                        left_sibling.next_leaf = child.next_leaf;
                    } else {
                        strcpy(left_sibling.keys[left_sibling.key_count], node.keys[idx - 1]);
                        left_sibling.key_count++;
                        for (int i = 0; i < child.key_count; i++) {
                            strcpy(left_sibling.keys[left_sibling.key_count + i], child.keys[i]);
                            left_sibling.children[left_sibling.key_count + i] = child.children[i];
                        }
                        left_sibling.children[left_sibling.key_count + child.key_count] = child.children[child.key_count];
                        left_sibling.key_count += child.key_count;
                    }
                    write_node(node.children[idx - 1], left_sibling);
                    // Remove key and child from node
                    for (int i = idx - 1; i < node.key_count - 1; i++) {
                        strcpy(node.keys[i], node.keys[i + 1]);
                    }
                    for (int i = idx; i < node.key_count; i++) {
                        node.children[i] = node.children[i + 1];
                    }
                    node.key_count--;
                    write_node(offset, node);
                } else if (idx < node.key_count) {
                    // Merge with right sibling
                    Node right_sibling = read_node(node.children[idx + 1]);
                    if (child.is_leaf) {
                        for (int i = 0; i < right_sibling.key_count; i++) {
                            strcpy(child.keys[child.key_count + i], right_sibling.keys[i]);
                            child.children[child.key_count + i] = right_sibling.children[i];
                        }
                        child.key_count += right_sibling.key_count;
                        child.next_leaf = right_sibling.next_leaf;
                    } else {
                        strcpy(child.keys[child.key_count], node.keys[idx]);
                        child.key_count++;
                        for (int i = 0; i < right_sibling.key_count; i++) {
                            strcpy(child.keys[child.key_count + i], right_sibling.keys[i]);
                            child.children[child.key_count + i] = right_sibling.children[i];
                        }
                        child.children[child.key_count + right_sibling.key_count] = right_sibling.children[right_sibling.key_count];
                        child.key_count += right_sibling.key_count;
                    }
                    write_node(node.children[idx], child);
                    // Remove key and child from node
                    for (int i = idx; i < node.key_count - 1; i++) {
                        strcpy(node.keys[i], node.keys[i + 1]);
                    }
                    for (int i = idx + 1; i < node.key_count + 1; i++) {
                        node.children[i] = node.children[i + 1];
                    }
                    node.key_count--;
                    write_node(offset, node);
                }

                underflow = (node.key_count < MIN_KEYS - 1);
                return true;
            }

            return true;
        }
    }

    void find_recursive(int64_t offset, const string& key, vector<int64_t>& result) {
        Node node = read_node(offset);

        if (node.is_leaf) {
            int idx = find_key_index(node, key);
            while (idx < node.key_count && compare_keys(node.keys[idx], key.c_str()) == 0) {
                result.push_back(node.children[idx]);
                idx++;
            }
        } else {
            int idx = find_key_index(node, key);
            find_recursive(node.children[idx], key, result);
        }
    }

public:
    BPT() {
        open_db();
    }

    ~BPT() {
        close_db();
    }

    void insert(const string& key, int64_t value) {
        if (header.root_offset == -1) {
            // Create root
            Node root;
            root.is_leaf = true;
            strcpy(root.keys[0], key.c_str());
            root.children[0] = value;
            root.key_count = 1;
            header.root_offset = allocate_node();
            write_node(header.root_offset, root);
            write_header();
            return;
        }

        string split_key;
        int64_t split_offset;
        bool split = insert_recursive(header.root_offset, key, value, split_key, split_offset);

        if (split) {
            // Create new root
            Node new_root;
            new_root.is_leaf = false;
            strcpy(new_root.keys[0], split_key.c_str());
            new_root.children[0] = header.root_offset;
            new_root.children[1] = split_offset;
            new_root.key_count = 1;
            int64_t new_root_offset = allocate_node();
            write_node(new_root_offset, new_root);
            header.root_offset = new_root_offset;
            write_header();
        }
    }

    void remove(const string& key, int64_t value) {
        if (header.root_offset == -1) return;

        bool underflow;
        delete_recursive(header.root_offset, key, value, underflow);

        // Check if root is underflowed
        Node root = read_node(header.root_offset);
        if (!root.is_leaf && root.key_count == 0) {
            // Promote only child as new root
            header.root_offset = root.children[0];
            write_header();
        }
    }

    void find(const string& key, vector<int64_t>& result) {
        if (header.root_offset == -1) return;
        find_recursive(header.root_offset, key, result);
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    BPT bpt;

    int n;
    cin >> n;

    for (int i = 0; i < n; i++) {
        string cmd;
        cin >> cmd;

        if (cmd == "insert") {
            string key;
            int64_t value;
            cin >> key >> value;
            bpt.insert(key, value);
        } else if (cmd == "delete") {
            string key;
            int64_t value;
            cin >> key >> value;
            bpt.remove(key, value);
        } else if (cmd == "find") {
            string key;
            cin >> key;
            vector<int64_t> result;
            bpt.find(key, result);

            if (result.empty()) {
                cout << "null" << endl;
            } else {
                sort(result.begin(), result.end());
                for (size_t j = 0; j < result.size(); j++) {
                    if (j > 0) cout << " ";
                    cout << result[j];
                }
                cout << endl;
            }
        }
    }

    return 0;
}
