import pandas as pd
import tkinter as tk
from tkinter import ttk
import math

# --------------------------
# 802.11a standard rate table
# --------------------------
rates = [
    {"Mbps": 6,  "Coding Rate": "1/2", "Rate_val": 1/2, "N_DBPS": 24},
    {"Mbps": 9,  "Coding Rate": "3/4", "Rate_val": 3/4, "N_DBPS": 36},
    {"Mbps": 12, "Coding Rate": "1/2", "Rate_val": 1/2, "N_DBPS": 48},
    {"Mbps": 18, "Coding Rate": "3/4", "Rate_val": 3/4, "N_DBPS": 72},
    {"Mbps": 24, "Coding Rate": "1/2", "Rate_val": 1/2, "N_DBPS": 96},
    {"Mbps": 36, "Coding Rate": "3/4", "Rate_val": 3/4, "N_DBPS": 144},
    {"Mbps": 48, "Coding Rate": "2/3", "Rate_val": 2/3, "N_DBPS": 192},
    {"Mbps": 54, "Coding Rate": "3/4", "Rate_val": 3/4, "N_DBPS": 216},
]

PACKET_SIZE = 1500  # bytes

def bits_to_bytes(bits):
    return round(bits / 8, 4)

# --------------------------
# Build table
# --------------------------
table = []

for r in rates:

    CRC_BYTES = PACKET_SIZE + 4
    L_CRC_bits = CRC_BYTES * 8

    # DATA before padding
    L_DATA_bits = 16 + L_CRC_bits + 6

    N_DBPS = r["N_DBPS"]

    N_SYM = math.ceil(L_DATA_bits / N_DBPS)

    # AFTER padding
    L_PPDU_bits = N_SYM * N_DBPS
    PAD_bits = L_PPDU_bits - L_DATA_bits

    L_PPDU_bytes = bits_to_bytes(L_PPDU_bits)

    Encoded_bytes = round(L_PPDU_bytes / r["Rate_val"], 4)

    table.append({
        "Rate (Mbps)": r["Mbps"],
        "Coding Rate": r["Coding Rate"],
        "N_DBPS": N_DBPS,
        "CRC (Bytes)": CRC_BYTES,
        "L_PPDU (Bytes)": L_PPDU_bytes,
        "Encoded (Bytes)": Encoded_bytes,
        "PAD (Bits)": PAD_bits,
        "Symbols": N_SYM
    })

df = pd.DataFrame(table)

# --------------------------
# GUI
# --------------------------
root = tk.Tk()
root.title("IEEE 802.11a PPDU + Encoded Size")
root.geometry("1050x400")
root.configure(bg="#f0f0f0")

style = ttk.Style()
style.theme_use("clam")

style.configure("Treeview",
                background="#ffffff",
                foreground="#000000",
                rowheight=28,
                fieldbackground="#ffffff",
                font=("Helvetica", 10))

style.configure("Treeview.Heading",
                font=("Helvetica", 11, "bold"),
                foreground="#ffffff",
                background="#2c3e50")

frame = ttk.Frame(root)
frame.pack(fill="both", expand=True, padx=10, pady=10)

tree = ttk.Treeview(frame, columns=list(df.columns), show="headings")
tree.pack(fill="both", expand=True, side="left")

scrollbar = ttk.Scrollbar(frame, orient="vertical", command=tree.yview)
scrollbar.pack(side="right", fill="y")
tree.configure(yscrollcommand=scrollbar.set)

for col in df.columns:
    tree.heading(col, text=col)
    tree.column(col, width=130, anchor="center")

for i, (_, row) in enumerate(df.iterrows()):
    tag = "evenrow" if i % 2 == 0 else "oddrow"
    tree.insert("", "end", values=list(row), tags=(tag,))

tree.tag_configure("evenrow", background="#eaf2f8")
tree.tag_configure("oddrow", background="#ffffff")

root.mainloop()
