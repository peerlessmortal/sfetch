# sfetch

`sfetch` is a lightweight, C-based terminal utility that combines a **neofetch-style system info / task stats display** with a retro ASCII **radar chart** driven by your `todo.txt` completion history.

It is designed to work seamlessly alongside [tuxedo](https://github.com/webstonehq/tuxedo) or any other standard `todo.txt-cli` compatible tool.

---

## Features

* **ASCII Radar Chart:** Renders a 5-axis spider/radar chart directly in your terminal using Unicode glyphs and 256-color ANSI depth shading.
* **Persona-Style Attributes:** Tracks 5 core stats by default:
  * 🧠 **Knowledge** (`!k`)
  * ❤️ **Vitality** (`!v`)
  * ⚡ **Diligence** (`!d`)
  * ✨ **Charm** (`!c`)
  * 🛠️ **Proficiency** (`!p`)
* **Level-Driven Progression:** Stats accumulate points via task markers, mapping to a confident-style Level system (Levels 1–6) with exponential thresholds. Level 1 is always the permanent floor.
* **Flexible Parser:** Scans completed tasks (`x `-prefixed lines) in `todo.txt` and `done.txt`, handling multiple whitespace-separated attribute flags on a single line (e.g., `!k !d`).
* **Dual Display Modes:** View your live system specs alongside the chart by default, or switch to `-t` mode to inspect your attribute bars and task completion counts.

---

## Installation & Building

Ensure you have a C compiler (`gcc`), `make`, and a terminal that supports UTF-8 and 256-color ANSI escape sequences.

```bash
# Clone or copy the source files into a directory
make

# Optional: Install globally to /usr/local/bin
sudo make install
```
