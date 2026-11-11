import sys, tkinter as tk
variant = sys.argv[1]
if variant == "tk":
    root = tk.Tk(); root.withdraw()
elif variant == "ctk":
    import customtkinter as ctk
    ctk.set_appearance_mode("light"); root = ctk.CTk(); root.withdraw()
elif variant == "ctk-widgets":
    import customtkinter as ctk
    ctk.set_appearance_mode("light"); root = ctk.CTk(); root.withdraw()
    root.title("x"); root.geometry("1000x700"); ctk.CTkButton(root, text="a").pack(); ctk.CTkEntry(root).pack()
if variant == "tk-selflock":
    import ctypes
    ok = ctypes.windll.user32.LockSetForegroundWindow(1)
    root = tk.Tk(); root.withdraw(); root.update()
    ctypes.windll.user32.LockSetForegroundWindow(2)
elif variant == "tk-nowrapper":
    # never let Tk make the wrapper while in the background: no update, no mainloop of Tk's own
    root = tk.Tk(); root.withdraw()
root.mainloop()
