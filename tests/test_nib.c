#include "test.h"

#include <stdlib.h>

#include "nib.h"
#include "util.h"

/* The Welcome window as MONSTER FAIR's nib has it, written out here: one
   button defined outside the window and referenced, the rest inline. */
static const char welcome_xib[] =
    "<?xml version=\"1.0\" standalone=\"yes\"?>\n"
    "<object class=\"NSIBObjectData\">\n"
    "  <object name=\"rootObject\" class=\"NSCustomObject\" id=\"1\">\n  </object>\n"
    "  <array count=\"3\" name=\"allObjects\">\n"
    "    <object class=\"IBCarbonButton\" id=\"206\">\n"
    "      <ostype name=\"command\">ok  </ostype>\n"
    "      <int name=\"buttonType\">1</int>\n"
    "      <string name=\"title\">Play Demo</string>\n"
    "      <string name=\"bounds\">104 20 124 122 </string>\n"
    "    </object>\n"
    "    <object class=\"IBCarbonMenuItem\" id=\"130\">\n"
    "      <string name=\"title\">Close</string>\n"
    "    </object>\n"
    "    <object class=\"IBCarbonWindow\" id=\"200\">\n"
    "      <int name=\"carbonWindowClass\">4</int>\n"
    "      <string name=\"title\">Welcome</string>\n"
    "      <object name=\"rootControl\" class=\"IBCarbonRootControl\" id=\"201\">\n"
    "        <array count=\"4\" name=\"subviews\">\n"
    "          <reference idRef=\"206\"/>\n"
    "          <object class=\"IBCarbonButton\" id=\"204\">\n"
    "            <ostype name=\"command\">not!</ostype>\n"
    "            <int name=\"buttonType\">2</int>\n"
    "            <string name=\"title\">Quit</string>\n"
    "            <string name=\"bounds\">104 388 124 490 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonStaticText\" id=\"207\">\n"
    "            <string name=\"title\">Thank you for trying.&#10;Click &quot;Buy Now&quot; &amp; more.</string>\n"
    "            <string name=\"bounds\">28 101 76 456 </string>\n"
    "          </object>\n"
    "          <object class=\"IBCarbonImageView\" id=\"202\">\n"
    "            <ostype name=\"controlSignature\">Appl</ostype>\n"
    "            <int name=\"controlID\">128</int>\n"
    "            <string name=\"bounds\">20 20 84 84 </string>\n"
    "          </object>\n"
    "        </array>\n"
    "        <string name=\"bounds\">0 0 144 510 </string>\n"
    "      </object>\n"
    "      <string name=\"windowRect\">190 257 334 767 </string>\n"
    "    </object>\n"
    "  </array>\n"
    "  <dictionary count=\"2\" name=\"nameTable\">\n"
    "    <string>File&apos;s Owner</string>\n"
    "    <reference idRef=\"1\"/>\n"
    "    <string>Welcome</string>\n"
    "    <reference idRef=\"200\"/>\n"
    "  </dictionary>\n"
    "</object>\n";

TEST(nib_reads_the_welcome_window) {
    nib_window w;
    char err[256] = "";
    CHECK(nib_read_window(welcome_xib, sizeof welcome_xib - 1, "Welcome", &w, err, sizeof err));
    CHECK_STR(w.name, "Welcome");
    CHECK_STR(w.title, "Welcome");
    CHECK_EQ(w.window_class, 4);
    CHECK_EQ(w.rect.top, 190);
    CHECK_EQ(w.rect.right, 767);
    CHECK_EQ(w.ncontrols, 4);
    const nib_control *play = &w.controls[0];
    CHECK_EQ(play->kind, NIB_BUTTON);
    CHECK_STR(play->title, "Play Demo");
    CHECK_EQ(play->command, FOURCC('o', 'k', ' ', ' '));
    CHECK_EQ(play->button_type, 1);
    CHECK_EQ(play->bounds.top, 104);
    CHECK_EQ(play->bounds.left, 20);
    CHECK_EQ(play->bounds.bottom, 124);
    CHECK_EQ(play->bounds.right, 122);
    CHECK_EQ(w.controls[1].command, FOURCC('n', 'o', 't', '!'));
    CHECK_EQ(w.controls[2].kind, NIB_STATIC_TEXT);
    CHECK_STR(w.controls[2].title, "Thank you for trying.\nClick \"Buy Now\" & more.");
    CHECK_EQ(w.controls[3].kind, NIB_IMAGE_VIEW);
    CHECK_EQ(w.controls[3].signature, FOURCC('A', 'p', 'p', 'l'));
    CHECK_EQ(w.controls[3].id, 128);
}

TEST(nib_says_which_window_is_missing) {
    nib_window w;
    char err[256] = "";
    CHECK(!nib_read_window(welcome_xib, sizeof welcome_xib - 1, "Register", &w, err, sizeof err));
    CHECK_CONTAINS(err, "no window called Register");
    CHECK(!nib_read_window(welcome_xib, sizeof welcome_xib - 1, "File's Owner", &w, err, sizeof err));
    CHECK_CONTAINS(err, "is a NSCustomObject, not a window");
}

TEST(nib_refuses_an_unknown_control) {
    char *xml = strdup(welcome_xib);
    char *s = strstr(xml, "IBCarbonStaticText");
    memcpy(s, "IBCarbonSliderXXXX", 18);
    nib_window w;
    char err[256] = "";
    CHECK(!nib_read_window(xml, strlen(xml), "Welcome", &w, err, sizeof err));
    CHECK_CONTAINS(err, "control 207 is an IBCarbonSliderXXXX, which isn't supported");
    free(xml);
}

TEST(nib_refuses_broken_xml) {
    nib_window w;
    char err[256] = "";
    const char *bad[] = {
        "<object class=\"NSIBObjectData\">",
        "<object class=\"NSIBObjectData\"></array>",
        "<object class=NSIBObjectData></object>",
        "<object>&bogus;</object>",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err[0] = '\0';
        CHECK(!nib_read_window(bad[i], strlen(bad[i]), "Welcome", &w, err, sizeof err));
        CHECK(err[0] != '\0');
    }
}

TEST(nib_reads_monster_fairs_windows) {
    SKIP_UNLESS_MF();
    char path[1200];
    snprintf(path, sizeof path, "%s/Contents/Resources/English.lproj/main.nib/objects.xib",
             test_mf_app());
    size_t len;
    char *xml = (char *)read_file(path, &len);
    CHECK(xml != NULL);
    const char *names[] = {"Welcome", "Register", "Demo", "AuthorizeFailed", "ThankYou", "MainWindow"};
    const int counts[] = {6, 8, 7, 3, 3, 0};
    for (int i = 0; i < 6; i++) {
        nib_window w;
        char err[256] = "";
        if (!nib_read_window(xml, len, names[i], &w, err, sizeof err))
            fprintf(stderr, "  %s: %s\n", names[i], err);
        CHECK(err[0] == '\0');
        CHECK_EQ(w.ncontrols, counts[i]);
    }
    nib_window reg;
    char err[256];
    CHECK(nib_read_window(xml, len, "Register", &reg, err, sizeof err));
    int edits = 0;
    for (int i = 0; i < reg.ncontrols; i++)
        if (reg.controls[i].kind == NIB_EDIT_TEXT) {
            CHECK_EQ(reg.controls[i].signature, FOURCC('U', 's', 'e', 'r'));
            CHECK_EQ(reg.controls[i].id, edits);
            edits++;
        }
    CHECK_EQ(edits, 2);
    free(xml);
}
