#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <ctime>
#include <sstream>
#include <iomanip>

using namespace std;

string calculateHash(int index, string previousHash, long long timestamp, string data, int nonce) {
    stringstream ss;
    ss << index << previousHash << timestamp << data << nonce;
    string input = ss.str();
    unsigned long hash = 5381;
    for(char c : input) {
        hash = ((hash << 5) + hash) + c;
    }
    stringstream finalHash;
    finalHash << hex << hash;
    return finalHash.str();
}

class Block {
public:
    int index;
    long long timestamp;
    string data;
    string previousHash;
    string hash;
    int nonce;

    Block(int idx, string d, string prevHash) {
        index = idx;
        timestamp = time(0);
        data = d;
        previousHash = prevHash;
        nonce = 0;
        hash = calculateHash(index, previousHash, timestamp, data, nonce);
    }
};

class Blockchain {
public:
    vector<Block> chain;
    vector<string> mempool;

    Blockchain() {
        chain.push_back(Block(0, "Genesis Block - Tazvera Core v6.0", "0"));
    }

    void addTransaction(string tx) {
        mempool.push_back(tx);
        cout << "TRANSACTION ADDED TO MEMPOOL: " << tx << endl;
    }

    void minePendingTransactions(string validator) {
        if (mempool.empty()) {
            cout << "No transactions in mempool to mine!" << endl;
            return;
        }

        string combinedData = "";
        for (string tx : mempool) {
            combinedData += tx + " | ";
        }
        combinedData += "Miner: " + validator;

        Block lastBlock = chain.back();
        Block newBlock(chain.size(), combinedData, lastBlock.hash);

        while (newBlock.hash.substr(0, 2) != "00") {
            newBlock.nonce++;
            newBlock.hash = calculateHash(newBlock.index, newBlock.previousHash, newBlock.timestamp, newBlock.data, newBlock.nonce);
        }

        chain.push_back(newBlock);
        mempool.clear();
        cout << "BLOCK MINED SUCCESSFULLY! Hash: " << newBlock.hash << endl;
        saveToJson();
    }

    void saveToJson() {
        ofstream file("tazvera_chain.json");
        file << "{\n  \"chain\": [\n";
        for (size_t i = 0; i < chain.size(); ++i) {
            file << "    {\n";
            file << "      \"index\": " << chain[i].index << ",\n";
            file << "      \"timestamp\": " << chain[i].timestamp << ",\n";
            file << "      \"data\": \"" << chain[i].data << "\",\n";
            file << "      \"previousHash\": \"" << chain[i].previousHash << "\",\n";
            file << "      \"hash\": \"" << chain[i].hash << "\",\n";
            file << "      \"nonce\": " << chain[i].nonce << "\n";
            file << "    }" << (i == chain.size() - 1 ? "" : ",") << "\n";
        }
        file << "  ]\n}\n";
        file.close();
        cout << "Blockchain saved to tazvera_chain.json successfully!" << endl;
    }
};

int main() {
    Blockchain tazvera;
    int choice;
    string tx, validator;

    do {
        cout << "\n--- TAZVERA BLOCKCHAIN CORE v6.0 ---\n";
        cout << "1. Add Transaction\n";
        cout << "2. Mine Block\n";
        cout << "3. Exit\n";
        cout << "Enter your choice: ";
        cin >> choice;

        switch(choice) {
            case 1:
                cout << "Enter transaction details: ";
                cin.ignore();
                getline(cin, tx);
                tazvera.addTransaction(tx);
                break;
            case 2:
                cout << "Enter validator name: ";
                cin.ignore();
                getline(cin, validator);
                tazvera.minePendingTransactions(validator);
                break;
            case 3:
                cout << "Exiting...\n";
                break;
            default:
                cout << "Invalid choice!\n";
        }
    } while(choice != 3);

    return 0;
}