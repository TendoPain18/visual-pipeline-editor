import os

# Set the folder you want to scan
folder_path = r"C:\Users\amrga\Downloads\final\pipeline-editor"
output_file = os.path.join(folder_path, "project_files.txt")

with open(output_file, "w", encoding="utf-8") as out_file:
    for root, dirs, files in os.walk(folder_path):
        # Skip folders
        for skip_dir in ["node_modules", "dist", ".vite", "Output_Files", "Test_Files", ".git"]:
            if skip_dir in dirs:
                dirs.remove(skip_dir)

        # Skip specific files
        skip_files = {
            "package-lock.json",
            "pipeline_diagram.psd",
            "project_files - Copy.txt",
            "icon.ico",
            ".gitignore",
            "test.py",
            "project_files.txt",
            "block_template2",
            "block_template3",
            "block_template4",
            "pipe_server.cpp",
            "pipe_server.exe",
            "pipeline_config.h",
            "pipeline_mex.mexw64",
        }

        files = [f for f in files if f not in skip_files]

        # 🔴 Skip ANY .exe file
        files = [f for f in files if not f.lower().endswith(".exe")]

        for file in files:
            file_path = os.path.join(root, file)
            try:
                out_file.write(f"--- FILE: {file_path} ---\n")
                with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                    out_file.write(content + "\n\n")
            except Exception as e:
                out_file.write(f"*** ERROR reading {file_path}: {e} ***\n\n")

print(f"All files saved to: {output_file}")
