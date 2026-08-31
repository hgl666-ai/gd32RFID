import fitz

pdf_path = r"c:\Users\Administrator\Desktop\RFID\FM15L013技术手册.pdf"
doc = fitz.open(pdf_path)

out = []
out.append(f"Total pages: {doc.page_count}\n")

# 输出每一页的全部文本，带页码分隔
for page_num in range(doc.page_count):
    page = doc[page_num]
    text = page.get_text()
    out.append(f"\n{'='*80}")
    out.append(f"===== PAGE {page_num + 1} =====")
    out.append(f"{'='*80}\n")
    out.append(text)

with open(r"c:\Users\Administrator\Desktop\RFID\pdf_full_text.txt", "w", encoding="utf-8") as f:
    f.write('\n'.join(out))

doc.close()
print("Done. Full text written to pdf_full_text.txt")
