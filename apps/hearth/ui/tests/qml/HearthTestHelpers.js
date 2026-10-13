// Deliberately NOT `.pragma library`: a library script has no imports of its
// own, so `item.Accessible` (an attached property) reads as undefined inside
// it. Imported plainly, the script shares each test file's own imports.
//
// Item-tree lookups the Hearth suites share. TestCase.findChild() only finds
// by objectName, and most of Hearth's controls carry none - a shared
// SegmentedControl's cells are all "seg-<value>" (so two controls with a
// "system" cell collide), and AppCheckBox/AppSlider/AppTextField rows are
// named by the label beside them. These walk the visual tree (children, and
// a Popup's contentItem where one is met) and match on what a person would
// read off the screen instead: an accessible name, a label.

// Depth-first over `item` and every visual descendant; the first for which
// `predicate` holds, or null.
function find(item, predicate) {
    if (item === null || item === undefined) {
        return null;
    }
    if (predicate(item)) {
        return item;
    }
    const kids = item.children;
    if (kids !== undefined) {
        for (let i = 0; i < kids.length; ++i) {
            const found = find(kids[i], predicate);
            if (found !== null) {
                return found;
            }
        }
    }
    return null;
}

// Every match, in tree order.
function findAll(item, predicate, out) {
    const results = out === undefined ? [] : out;
    if (item === null || item === undefined) {
        return results;
    }
    if (predicate(item)) {
        results.push(item);
    }
    const kids = item.children;
    if (kids !== undefined) {
        for (let i = 0; i < kids.length; ++i) {
            findAll(kids[i], predicate, results);
        }
    }
    return results;
}

// A visible SegmentedControl by its accessibleName, then its "seg-<value>"
// cell. Visible, because a page can hold two controls with the same name on
// sub-pages only one of which is showing (DecoderPage.qml's E-AC-3 and AC-4
// "Mode").
function segment(root, accessibleName, value) {
    const control = find(root, function(item) {
        return item.accessibleName === accessibleName && item.model !== undefined && item.selected !== undefined
               && item.visible;
    });
    if (control === null) {
        return null;
    }
    return find(control, function(item) { return item.objectName === "seg-" + value; });
}

// An AppCheckBox by its label text.
function checkBox(root, text) {
    return find(root, function(item) {
        return item.text === text && item.toggled !== undefined && item.note !== undefined && item.visible;
    });
}

// A control whose Accessible.name is `name` (a slider, a text field, a
// button) - Accessible.name is readable from QML as item.Accessible.name.
function byAccessibleName(root, name) {
    return find(root, function(item) {
        return item.Accessible !== undefined && item.Accessible.name === name && item.visible;
    });
}

// A visible Text whose text is exactly `text`.
function textItem(root, text) {
    return find(root, function(item) {
        return item.text === text && item.font !== undefined && item.visible;
    });
}

// A Text whose text contains `fragment`.
function textContaining(root, fragment) {
    return find(root, function(item) {
        return typeof item.text === "string" && item.font !== undefined && item.visible
               && item.text.indexOf(fragment) >= 0;
    });
}

// What under `root` is laid out or drawn past root's left or right edge, one
// line of text for each: every visible item by its box, and a Text by what it
// draws as well, since a Text draws past a box too narrow for it unless it
// wraps or elides. An item that clips is taken by its own box and not looked
// into: what it cuts off is its own business (a lyric view's long line). The
// one that spans `root` from edge to edge is root's own viewport, a
// ScrollView's Flickable, and what that cuts off is what is being asked.
// Empty when `root` holds all of it, which is what a page owes its window.
function pastTheEdges(root) {
    const found = [];
    const visit = function(item) {
        if (item === null || item === undefined || !item.visible) {
            return;
        }
        let left = item.mapToItem(root, 0, 0).x;
        let right = left + item.width;
        if (item.elide !== undefined && item.contentWidth > item.width) {
            const spill = item.contentWidth - item.width;
            const alignment = item.effectiveHorizontalAlignment;
            if (alignment === Qt.AlignRight) {
                left -= spill;
            } else if (alignment === Qt.AlignHCenter) {
                left -= spill / 2;
                right += spill / 2;
            } else {
                right += spill;
            }
        }
        if (left < -0.5 || right > root.width + 0.5) {
            const name = item.objectName
                ? item.objectName
                : (typeof item.text === "string" && item.text.length > 0
                   ? "\"" + item.text.substring(0, 32) + "\"" : String(item));
            found.push(name + " at " + Math.round(left) + " to " + Math.round(right));
        }
        const viewport = left <= 0.5 && right >= root.width - 0.5;
        if (item !== root && item.clip === true && !viewport) {
            return;
        }
        const kids = item.children;
        if (kids !== undefined) {
            for (let i = 0; i < kids.length; ++i) {
                visit(kids[i]);
            }
        }
    };
    visit(root);
    return found;
}

// The non-visual QtQuick.Dialogs FileDialog/FolderDialog `owner` declares,
// by its title (PlayPage.qml has two). A dialog is not a visual child, and a
// ScrollView-rooted page lists it nowhere, so this asks the QObject tree
// (TestServices.findByProperty(), qml_test_main.cpp) - which is why it needs
// the TestServices singleton handed in.
function dialog(owner, titleText, services) {
    const found = services.findByProperty(owner, "title", titleText);
    return found !== null && found.accept !== undefined && found.currentFolder !== undefined ? found : null;
}

// file:///a/b/c.ec3 for /a/b/c.ec3.
function fileUrl(path) {
    return "file://" + (path.charAt(0) === "/" ? "" : "/") + path;
}

function folderOf(path) {
    return path.substring(0, path.lastIndexOf("/"));
}

// The control sitting in the same row as the label `labelText` - for a
// slider or field whose only name is the Text beside it (DecoderEac3.qml's
// "Cut"/"Boost"). `predicate` picks which sibling.
function besideLabel(root, labelText, predicate) {
    const label = textItem(root, labelText);
    if (label === null || label.parent === null) {
        return null;
    }
    const siblings = label.parent.children;
    for (let i = 0; i < siblings.length; ++i) {
        if (siblings[i] !== label && predicate(siblings[i])) {
            return siblings[i];
        }
    }
    return null;
}

function isSlider(item) {
    return item.from !== undefined && item.to !== undefined && item.visualPosition !== undefined;
}
