import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ZiFiConsoleInitContractTest(unittest.TestCase):
    def test_photo_garbage_is_status_bitmap_data(self) -> None:
        menu = (ROOT / "ZiFi SPG" / "_spg" / "menu.tga.pix").read_bytes()
        photographed = b"[WUWUWU[=)xx(KKJ"

        self.assertEqual(menu[0x2CF0 : 0x2CF0 + len(photographed)], photographed)

    def test_text_page_is_cleared_without_dma_before_interrupts(self) -> None:
        source = (ROOT / "ZiFi SPG" / "zifi.asm").read_text(encoding="utf-8")
        clear_start = source.index("\nset_text_colors\n") + 1
        clear_end = source.index("create_link_list\n", clear_start)
        clear_routine = source[clear_start:clear_end]

        self.assertIn("ld bc,#1fff", clear_routine)
        self.assertIn("ld (hl),a\n\t\tldir", clear_routine)
        self.assertNotIn("clr_text_screen", clear_routine)

        startup = source[source.index("\nstart\n") : source.index("\nmain\n")]
        ordered_steps = (
            "\n\t\tdi\n",
            "call gfx_init",
            "call set_text_colors",
            "ld hl,int_main",
            "ld (#beff),hl",
            "\n\t\tei\n",
        )
        cursor = 0
        for step in ordered_steps:
            cursor = startup.index(step, cursor) + len(step)


if __name__ == "__main__":
    unittest.main()
