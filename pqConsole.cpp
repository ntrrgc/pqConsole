/*
    pqConsole    : interfacing SWI-Prolog and Qt

    Author       : Carlo Capelli
    E-mail       : cc.carlo.cap@gmail.com
    Copyright (C): 2013, Carlo Capelli

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

// by now peek system namespace. Eventually, will move to pqConsole
#define PROLOG_MODULE "system"

#include <SWI-Stream.h>

#include "pqConsole.h"
#include "pqMainWindow.h"
#include "ConsoleEdit.h"
#include "Swipl_IO.h"
#include "PREDICATE.h"
#include "do_events.h"
#include "Preferences.h"

#include <QApplication>
#include <QMainWindow>
#include <QStack>
#include <QFontMetrics>

#include <QMetaProperty>
#include <QMetaObject>
#include <QDebug>
#include <QMenuBar>

#include <QFileDialog>
#include <QFontDialog>
#include <QClipboard>

/** Run a default GUI to demo the ability to embed Prolog with minimal effort.
 *  It will evolve - eventually - from a demo
 *  to the *official* SWI-Prolog console in main distribution - Wow
 */
int pqConsole::runDemo(int argc, char *argv[]) {
    QApplication a(argc, argv);
    pqMainWindow w(argc, argv);
    w.show();
    return a.exec();
}

/** standard constructor, generated by QtCreator.
 */
pqConsole::pqConsole() {
}

/** depth first search of widgets hierarchy, from application topLevelWidgets
 */
static QWidget *search_widget(std::function<bool(QWidget* w)> match) {
    foreach (auto widget, QApplication::topLevelWidgets()) {
        QStack<QObject*> s;
        s.push(widget);
        while (!s.isEmpty()) {
            auto p = qobject_cast<QWidget*>(s.pop());
            if (match(p))
                return p;
            foreach (auto c, p->children())
                if (c->isWidgetType())
                    s.push(c);
        }
    }
    return 0;
}

/** search widgets hierarchy looking for the first (the only)
 *  that owns the calling thread ID
 */
static ConsoleEdit *console_by_thread() {
    int thid = PL_thread_self();
    return qobject_cast<ConsoleEdit*>(search_widget([=](QWidget* p) {
        if (auto ce = qobject_cast<ConsoleEdit*>(p))
            return ce->match_thread(thid);
        return false;
    }));
}

/** search widgets hierarchy looking for any ConsoleEdit
 */
static ConsoleEdit *console_peek_first() {
    return qobject_cast<ConsoleEdit*>(search_widget([](QWidget* p) {
        return qobject_cast<ConsoleEdit*>(p) != 0;
    }));
}

/** unify a property of QObject:
 *  allows read/write of simple atomic values
 */
static void unify(const QMetaProperty& p, QObject *o, PlTerm v) {

    switch (v.type()) {

    case PL_VARIABLE:
        switch (p.type()) {
        case QVariant::Bool:
            v = p.read(o).toBool() ? A("true") : A("false");
            return;
        case QVariant::Int:
            if (p.isEnumType()) {
                Q_ASSERT(!p.isFlagType());  // TBD
                QMetaEnum e = p.enumerator();
                if (CCP key = e.valueToKey(p.read(o).toInt())) {
                    v = A(key);
                    return;
                }
            }
            v = long(p.read(o).toInt());
            return;
        case QVariant::UInt:
            v = long(p.read(o).toUInt());
            return;
        case QVariant::String:
            v = A(p.read(o).toString());
            return;
        default:
            break;
        }
        break;

    case PL_INTEGER:
        switch (p.type()) {
        case QVariant::Int:
        case QVariant::UInt:
            if (p.write(o, qint32(v)))
                return;
        default:
            break;
        }
        break;

    case PL_ATOM:
        switch (p.type()) {
        case QVariant::String:
            if (p.write(o, CCP(v)))
                return;
        case QVariant::Int:
            if (p.isEnumType()) {
                Q_ASSERT(!p.isFlagType());  // TBD
                int i = p.enumerator().keyToValue(v);
                if (i != -1) {
                    p.write(o, i);
                    return;
                }
            }
        default:
            break;
        }
        break;

    case PL_FLOAT:
        switch (p.type()) {
        case QVariant::Double:
            if (p.write(o, double(v)))
                return;
        default:
            break;
        }
        break;

    default:
        break;
    }

    throw PlException(A("property type mismatch"));
}

// SWIPL-WIN.EXE interface implementation

/** window_title(-Old, +New)
 *  get/set console title
 */
PREDICATE(window_title, 2) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QWidget *w = c->parentWidget();
        if (qobject_cast<QMainWindow*>(w)) {
            A1 = A(w->windowTitle());
            w->setWindowTitle(CCP(A2));
            return TRUE;
        }
    }
    return FALSE;
}

/** win_window_pos(Options)
 *  Option:
 *     size(W, H)
 *     position(X, Y)
 *     zorder(ZOrder)
 *     show(Bool)
 *     activate
 */
PREDICATE(win_window_pos, 1) {
    ConsoleEdit* c = console_by_thread();
    if (!c)
        return FALSE;

    QWidget *w = c->parentWidget();
    if (!w)
        return FALSE;

    T opt;
    L options(A1);
    typedef QPair<int, QString> O;
    while (options.next(opt)) {
        O o = O(opt.arity(), opt.name());
        if (o == O(2, "size")) {
            long W = opt[1], H = opt[2];
            QSize sz = c->fontMetrics().size(0, "Q");
            w->resize(sz.width() * W, sz.height() * H);
            continue;
        }
        if (o == O(2, "position")) {
            long X = opt[1], Y = opt[2];
            w->move(X, Y);
            continue;
        }
        if (o == O(1, "zorder")) {
            // TBD ...
            // long ZOrder = opt[1];
            continue;
        }
        if (o == O(1, "show")) {
            bool y = QString(opt[1].name()) == "true";
            if (y)
                w->show();
            else
                w->hide();
            continue;
        }
        if (o == O(0, "activate")) {
            w->activateWindow();
            continue;
        }

        // print_error
        return FALSE;
    }

    return TRUE;
}

/** win_has_menu
 *  true =only= when ConsoleEdit is directly framed inside a QMainWindow
 */
PREDICATE(win_has_menu, 0) { Q_UNUSED(_av);
    auto ce = console_by_thread();
    return ce && qobject_cast<QMainWindow*>(ce->parentWidget()) ? TRUE : FALSE;
}

/** MENU interface
 *  helper to lookup position and issue action creation
 */
static QAction* add_action(ConsoleEdit *ce, QMenu *mn, QString Label, QString ctxtmod, QString Goal, QAction *before = 0) {
    auto a = new QAction(mn);
    a->setText(Label);
    a->setToolTip(ctxtmod + ':' + Goal);  // use as spare storage for Module:Goal
    QObject::connect(a, SIGNAL(triggered()), ce, SLOT(onConsoleMenuAction()));
    if (before)
        mn->insertAction(before, a);
    else
        mn->addAction(a);
    return a;
}

/** win_insert_menu(+Label, +Before)
 *  do action construction
 */
PREDICATE(win_insert_menu, 2) {
    if (ConsoleEdit *ce = console_by_thread()) {
        QString Label = CCP(A1), Before = CCP(A2);
        ce->exec_func([=]() {
            if (auto mw = qobject_cast<QMainWindow*>(ce->parentWidget())) {
                auto mbar = mw->menuBar();
                foreach (QAction *ac, mbar->actions())
                    if (ac->text() == Label)
                        return;
                foreach (QAction *ac, mbar->actions())
                    if (ac->text() == Before) {
                        mbar->insertMenu(ac, new QMenu(Label));
                        return;
                    }
                if (Before == "-") {
                    mbar->addMenu(Label);
                    return;
                }
            }
            qDebug() << "failed win_insert_menu" << Label << Before;
        });
        return TRUE;
    }
    return FALSE;
}

/** win_insert_menu_item(+Pulldown, +Label, +Before, :Goal)
 *  does search insertion position and create menu item
 */
PREDICATE(win_insert_menu_item, 4) {

    if (ConsoleEdit *ce = console_by_thread()) {
        QString Pulldown = CCP(A1), Label = CCP(A2), Before = CCP(A3), Goal = CCP(A4);
        //qDebug() << "win_insert_menu_item" << Pulldown << Label << Before << Goal;

        QString ctxtmod = CCP(PlAtom(PL_module_name(PL_context())));
        // if (PlCall("context_module", cx)) ctxtmod = CCP(cx); -- same as above: system
        ctxtmod = "win_menu";

        ce->exec_func([=]() {
            if (auto mw = qobject_cast<QMainWindow*>(ce->parentWidget())) {
                foreach (QAction *ac, mw->menuBar()->actions())
                    if (ac->text() == Pulldown) {
                        QMenu *mn = ac->menu();
                        if (Label != "--")
                            foreach (QAction *bc, ac->menu()->actions())
                                if (bc->text() == Label) {
                                    bc->setToolTip(Goal);
                                    return;
                                }
                        if (Before == "-") {
                            if (Label == "--")
                                mn->addSeparator();
                            else
                                add_action(ce, mn, Label, ctxtmod, Goal);
                            return;
                        }
                        foreach (QAction *bc, ac->menu()->actions())
                            if (bc->text() == Before) {
                                if (Label == "--")
                                    mn->insertSeparator(bc);
                                else
                                    add_action(ce, mn, Label, ctxtmod, Goal, bc);
                                return;
                            }

                        QAction *bc = add_action(ce, mn, Before, ctxtmod, "");
                        add_action(ce, mn, Label, ctxtmod, Goal, bc);
                    }
            }
        });
        return TRUE;
    }
    return FALSE;
}

/** tty_clear
 *  as requested by Annie. Should as well be implemented capturing ANSI terminal sequence
 */
PREDICATE(tty_clear, 0) { Q_UNUSED(_av);
    ConsoleEdit* c = console_by_thread();
    if (c) {
        c->tty_clear();
        return TRUE;
    }
    return FALSE;
}

/** win_open_console(Title, In, Out, Err, [ registry_key(Key) ])
 *  code stolen - verbatim - from pl-ntmain.c
 *  registry_key(Key) unused by now
 */
PREDICATE(win_open_console, 5) {

    qDebug() << "win_open_console" << CVP(CT);

    ConsoleEdit *ce = console_peek_first();
    if (!ce)
        throw PlException(A("no ConsoleEdit available"));

    static IOFUNCTIONS rlc_functions = {
        Swipl_IO::_read_f,
        Swipl_IO::_write_f,
        Swipl_IO::_seek_f,
        Swipl_IO::_close_f,
        Swipl_IO::_control_f,
        Swipl_IO::_seek64_f
    };

    #define STREAM_COMMON (\
        SIO_TEXT|       /* text-stream */           \
        SIO_NOCLOSE|    /* do no close on abort */	\
        SIO_ISATTY|     /* terminal */              \
        SIO_NOFEOF)     /* reset on end-of-file */

    auto c = new Swipl_IO;
    IOSTREAM
        *in  = Snew(c,  SIO_INPUT|SIO_LBUF|STREAM_COMMON, &rlc_functions),
        *out = Snew(c, SIO_OUTPUT|SIO_LBUF|STREAM_COMMON, &rlc_functions),
        *err = Snew(c, SIO_OUTPUT|SIO_NBUF|STREAM_COMMON, &rlc_functions);

    in->position  = &in->posbuf;		/* record position on same stream */
    out->position = &in->posbuf;
    err->position = &in->posbuf;

    in->encoding  = ENC_UTF8;
    out->encoding = ENC_UTF8;
    err->encoding = ENC_UTF8;

    ce->new_console(c, CCP(A1));

    if (!PL_unify_stream(A2, in) ||
        !PL_unify_stream(A3, out) ||
        !PL_unify_stream(A4, err)) {
            Sclose(in);
            Sclose(out);
            Sclose(err);
        return FALSE;
    }

    return TRUE;
}

/** append new command to history list for current console
 */
PREDICATE(rl_add_history, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        WCP line = A1;
        if (*line)
            c->add_history_line(QString::fromWCharArray(line));
        return TRUE;
    }
    return FALSE;
}

/** this should only be used as flag to enable processing ?
 */
PREDICATE(rl_read_init_file, 1) {
    Q_UNUSED(_av);
    return TRUE;
}

/** get history lines for this console
 */
NAMED_PREDICATE($rl_history, rl_history, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        PlTail lines(A1);
        foreach(QString x, c->history_lines())
            lines.append(W(x));
        lines.close();
        return TRUE;
    }
    return FALSE;
}

/** attempt to overcome default tty_size/2
 */
PREDICATE(tty_size, 2) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QSize sz = c->fontMetrics().size(0, "Q");
        long Rows = c->height() / sz.height();
        long Cols = c->width() / sz.width();
        A1 = Rows;
        A2 = Cols;
        return TRUE;
    }
    return FALSE;
}

/** break looping
PREDICATE(interrupt, 0) { Q_UNUSED(_av);
    throw PlException(PlAtom("stop_req"));
    return FALSE;
}
*/

#undef PROLOG_MODULE
#define PROLOG_MODULE "pqConsole"

/** set/get settings of thread associated console
 *
 *  updateRefreshRate(N) default 100
 *  - allow to alter default refresh rate (simply count outputs before setting cursor at end)
 *
 *  maximumBlockCount(N) default 0
 *  - remove (from top) text lines when exceeding the limit
 *
 *  lineWrapMode(Mode) Mode --> 'NoWrap' | 'WidgetWidth'
 *  - set/get current line wrapping. When is off, an horizontal scroll bar could display
 */
PREDICATE(console_settings, 1) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        PlFrame fr;
        PlTerm opt;
        for (PlTail opts(A1); opts.next(opt); ) {
            if (opt.arity() == 1) {
                CCP name = opt.name();
                int pid = c->metaObject()->indexOfProperty(name);
                if (pid >= 0)
                    unify(c->metaObject()->property(pid), c, opt[1]);
                else
                    throw PlException(A("property not found"));
            }
            else
                throw PlException(A("properties have arity 1"));
        }
        return TRUE;
    }
    return FALSE;
}

/** getOpenFileName(+Title, ?StartPath, +Pattern, -Choice)
 *  run a modal dialog on request from foreign thread
 *  this must run a modal loop in GUI thread
 */
PREDICATE(getOpenFileName, 4) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QString Caption = CCP(A1), StartPath, Pattern = CCP(A3), Choice;
        if (A2.type() == PL_ATOM)
            StartPath = CCP(A2);

        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            Choice = QFileDialog::getOpenFileName(c, Caption, StartPath, Pattern);
            s.go();
        });
        s.stop();

        if (!Choice.isEmpty()) {
            A4 = A(Choice);
            return TRUE;
        }
    }
    return FALSE;
}

/** getSaveFileName(+Title, ?StartPath, +Pattern, -Choice)
 *  run a modal dialog on request from foreign thread
 *  this must run a modal loop in GUI thread
 */
PREDICATE(getSaveFileName, 4) {
    ConsoleEdit* c = console_by_thread();
    if (c) {
        QString Caption = CCP(A1), StartPath, Pattern = CCP(A3), Choice;
        if (A2.type() == PL_ATOM)
            StartPath = CCP(A2);

        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            Choice = QFileDialog::getSaveFileName(c, Caption, StartPath, Pattern);
            s.go();
        });
        s.stop();

        if (!Choice.isEmpty()) {
            A4 = A(Choice);
            return TRUE;
        }
    }
    return FALSE;
}

/** select_font
 *  run Qt font selection
 */
PREDICATE(select_font, 0) { Q_UNUSED(_av);
    ConsoleEdit* c = console_by_thread();
    bool ok = false;
    if (c) {
        ConsoleEdit::exec_sync s;
        c->exec_func([&]() {
            Preferences p;
            QFont font = QFontDialog::getFont(&ok, p.console_font, c);
            if (ok)
                c->setFont(p.console_font = font);
            s.go();
        });
        s.stop();
    }
    return ok;
}

/** quit_console
 *  just issue termination to Qt application object
 */
PREDICATE(quit_console, 0) { Q_UNUSED(_av);
    ConsoleEdit* c = console_by_thread();
    if (c) {
        c->exec_func([](){ qApp->quit(); });
        return TRUE;
    }
    return FALSE;
}

/** issue a copy to clipboard of current selection
 */
PREDICATE(copy, 0) { Q_UNUSED(_av);
    ConsoleEdit* c = console_by_thread();
    if (c) {
        c->exec_func([=](){
            QApplication::clipboard()->setText(c->textCursor().selectedText());
            do_events();
        });
        return TRUE;
    }
    return FALSE;
}

/** issue a paste to clipboard of current selection
 */
PREDICATE(paste, 0) { Q_UNUSED(_av);
    ConsoleEdit* c = console_by_thread();
    if (c) {
        c->exec_func([=](){
            c->textCursor().insertText(QApplication::clipboard()->text());
            do_events();
        });
        return TRUE;
    }
    return FALSE;
}
