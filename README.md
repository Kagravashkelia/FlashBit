# ⚡ FlashBit

A high-performance, custom C++ chess engine powered by advanced NNUE evaluation and optimized search algorithms.

---

## 🚀 About

**FlashBit** is a chess engine built from the ground up to explore calculation speed, tactical depth, and machine-learning-based position evaluation. Moving away from standard templates, FlashBit integrates modern **NNUE (Efficiently Updatable Neural Networks)** evaluation models alongside custom search logic to deliver sharp, competitive, and deeply calculated play.

---

## ✨ Key Features

* **NNUE Evaluation Support:** Utilizes specialized neural network weight files (`.nnue`) for rapid, accurate, and position-aware evaluations.
* **Custom Search Engine:** Features optimized search routines (such as custom Negamax implementations) designed to handle complex tactical trees efficiently.
* **High-Performance C++:** Written natively in C++ to guarantee maximum speed during deep game tree traversals and bitboard manipulations.
* **Modular Architecture:** Clean separation of core engine logic, search heuristics, and board representation layers.

---

## 📂 Project Structure

```text
FlashBit/
├── include/          # Header files, custom structures, and chess libraries
├── src/              # Core engine logic, search heuristics, and NNUE probe code
├── Makefile          # Compilation and build configuration
└── README.md         # Project documentation
