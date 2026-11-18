// Qt 5 on AXIOM: a small gallery of Qt's widgets, the qt5 package's own program (as GTK's demo is gtk3's).
// Built by build-packages.py:  g++ -O2 -fPIC qt5-demo.cpp -o qt5-demo $(pkg-config --cflags --libs Qt5Widgets)
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

static QWidget *controls() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *buttons = new QGroupBox("Buttons");
    auto *bl = new QHBoxLayout(buttons);
    auto *count = new QLabel("Clicked 0 times");
    auto *push = new QPushButton("Click me");
    QObject::connect(push, &QPushButton::clicked, [count] {
        static int n; count->setText(QString("Clicked %1 time%2").arg(++n).arg(n == 1 ? "" : "s"));
    });
    bl->addWidget(push); bl->addWidget(count); bl->addStretch();
    auto *choices = new QGroupBox("Choices");
    auto *cl = new QHBoxLayout(choices);
    cl->addWidget(new QCheckBox("Check box"));
    auto *r1 = new QRadioButton("Option A"); r1->setChecked(true);
    cl->addWidget(r1); cl->addWidget(new QRadioButton("Option B"));
    auto *combo = new QComboBox; combo->addItems({ "Small", "Medium", "Large" }); cl->addWidget(combo);
    cl->addStretch();
    auto *values = new QGroupBox("Values");
    auto *vl = new QHBoxLayout(values);
    auto *slider = new QSlider(Qt::Horizontal); slider->setRange(0, 100); slider->setValue(40);
    auto *spin = new QSpinBox; spin->setRange(0, 100); spin->setValue(40);
    auto *bar = new QProgressBar; bar->setRange(0, 100); bar->setValue(40);
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    QObject::connect(slider, &QSlider::valueChanged, bar, &QProgressBar::setValue);
    vl->addWidget(slider); vl->addWidget(spin); vl->addWidget(bar);
    auto *line = new QLineEdit; line->setPlaceholderText("Type something here");
    v->addWidget(buttons); v->addWidget(choices); v->addWidget(values); v->addWidget(line); v->addStretch();
    return w;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Qt 5");
    QWidget win;
    win.setWindowTitle("Qt 5 on AXIOM");
    auto *v = new QVBoxLayout(&win);
    auto *head = new QLabel(QString("<b>Qt %1</b> is installed. Qt apps run in AXIOM windows, through AXIOM's Wayland "
                                    "compositor.").arg(qVersion()));
    head->setWordWrap(true);
    auto *tabs = new QTabWidget;
    tabs->addTab(controls(), "Controls");
    auto *list = new QListWidget;
    for (const char *s : { "Apples", "Bananas", "Cherries", "Dates", "Elderberries", "Figs", "Grapes" }) list->addItem(s);
    tabs->addTab(list, "List");
    auto *text = new QTextEdit;
    text->setPlainText("A text editor widget: type, select, copy and paste.\n");
    tabs->addTab(text, "Text");
    auto *about = new QPushButton("About Qt");
    QObject::connect(about, &QPushButton::clicked, [&win] { QMessageBox::aboutQt(&win); });
    auto *row = new QHBoxLayout; row->addStretch(); row->addWidget(about);
    v->addWidget(head); v->addWidget(tabs); v->addLayout(row);
    win.resize(560, 420);
    win.show();
    return app.exec();
}
