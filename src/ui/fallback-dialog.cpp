// fallback-dialog.cpp
#include "ui/fallback-dialog.h"
#include "core/common.h"
#include "core/fallback-fit.h"
#include "core/fallback-layer.h"
#include "core/fallback-settings.h"

#include <obs-module.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstring>
#include <utility>

namespace lss {

static constexpr const char *IMAGE_FILTER = "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;All Files (*.*)";
static constexpr const char *VIDEO_FILTER = "Video (*.mp4 *.mov *.mkv *.webm *.ts *.flv *.gif);;All Files (*.*)";
static constexpr const char *PAGE_FILTER = "HTML (*.html *.htm);;All Files (*.*)";

static QString text(const char *key) {
  return QString::fromUtf8(obs_module_text(key));
}

// A colour with its opacity, kept the way OBS keeps it: 0xAABBGGRR.
class ColorButton : public QPushButton {
public:
  explicit ColorButton(const QString &title) : title_(title) {
    connect(this, &QPushButton::clicked, this, [this] {
      const QColor picked = QColorDialog::getColor(color(), this, title_,
                                                   QColorDialog::ShowAlphaChannel |
                                                       QColorDialog::DontUseNativeDialog);
      if (picked.isValid())
        set_value(static_cast<uint32_t>(picked.alpha()) << 24 | picked.blue() << 16 |
                  picked.green() << 8 | picked.red());
    });
  }

  uint32_t value() const { return abgr_; }

  void set_value(uint32_t abgr) {
    abgr_ = abgr;
    const QColor shown = color();

    // Squares behind the colour show how see-through it is.
    QPixmap swatch(28, 16);
    swatch.fill(Qt::white);
    QPainter painter(&swatch);
    for (int x = 0; x < swatch.width(); x += 4) {
      for (int y = (x / 4) % 2 * 4; y < swatch.height(); y += 8)
        painter.fillRect(x, y, 4, 4, Qt::lightGray);
    }
    painter.fillRect(swatch.rect(), shown);
    painter.end();

    setIcon(swatch);
    setIconSize(swatch.size());
    setText(QString("%1  %2%").arg(shown.name().toUpper()).arg(shown.alpha() * 100 / 255));
  }

private:
  QColor color() const {
    return QColor(abgr_ & 0xFF, (abgr_ >> 8) & 0xFF, (abgr_ >> 16) & 0xFF, abgr_ >> 24);
  }

  QString title_;
  uint32_t abgr_ = 0;
};

class FontButton : public QPushButton {
public:
  explicit FontButton(const QString &title) {
    connect(this, &QPushButton::clicked, this, [this, title] {
      bool picked = false;
      const QFont chosen = QFontDialog::getFont(&picked, font_, this, title,
                                                QFontDialog::DontUseNativeDialog);
      if (picked)
        set_font(chosen);
    });
  }

  void load(obs_data_t *font) {
    QFont loaded;
    if (font) {
      const uint32_t flags = static_cast<uint32_t>(obs_data_get_int(font, "flags"));
      loaded.setFamily(QString::fromUtf8(obs_data_get_string(font, "face")));
      loaded.setStyleName(QString::fromUtf8(obs_data_get_string(font, "style")));
      if (const int size = static_cast<int>(obs_data_get_int(font, "size")); size > 0)
        loaded.setPointSize(size);
      loaded.setBold(flags & OBS_FONT_BOLD);
      loaded.setItalic(flags & OBS_FONT_ITALIC);
      loaded.setUnderline(flags & OBS_FONT_UNDERLINE);
      loaded.setStrikeOut(flags & OBS_FONT_STRIKEOUT);
    }
    set_font(loaded);
  }

  obs_data_t *save() const {
    obs_data_t *font = obs_data_create();
    const uint32_t flags = (font_.bold() ? OBS_FONT_BOLD : 0) | (font_.italic() ? OBS_FONT_ITALIC : 0) |
                           (font_.underline() ? OBS_FONT_UNDERLINE : 0) |
                           (font_.strikeOut() ? OBS_FONT_STRIKEOUT : 0);
    obs_data_set_string(font, "face", font_.family().toUtf8().constData());
    obs_data_set_string(font, "style", font_.styleName().toUtf8().constData());
    obs_data_set_int(font, "size", font_.pointSize());
    obs_data_set_int(font, "flags", flags);
    return font;
  }

private:
  void set_font(const QFont &font) {
    font_ = font;
    setText(QString("%1, %2").arg(font_.family()).arg(font_.pointSize()));
  }

  QFont font_;
};

class PathRow : public QWidget {
public:
  explicit PathRow(const QString &title) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    edit_ = new QLineEdit();
    auto *browse = new QPushButton(text("FallbackDialogBrowse"));
    row->addWidget(edit_, 1);
    row->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this, title] {
      const QString file = QFileDialog::getOpenFileName(this, title, edit_->text(), filter_);
      if (!file.isEmpty())
        edit_->setText(file);
    });
  }

  void set_filter(const char *filter) { filter_ = QString::fromUtf8(filter); }
  std::string value() const { return edit_->text().toUtf8().constData(); }
  void set_value(const char *path) { edit_->setText(QString::fromUtf8(path)); }

private:
  QLineEdit *edit_ = nullptr;
  QString filter_;
};

class SliderRow : public QWidget {
public:
  SliderRow(int minimum, int maximum, int step, const QString &suffix) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    slider_ = new QSlider(Qt::Horizontal);
    spin_ = new QSpinBox();
    slider_->setRange(minimum, maximum);
    slider_->setSingleStep(step);
    slider_->setPageStep(step * 4);
    spin_->setRange(minimum, maximum);
    spin_->setSingleStep(step);
    spin_->setSuffix(suffix);
    row->addWidget(slider_, 1);
    row->addWidget(spin_);
    connect(slider_, &QSlider::valueChanged, spin_, &QSpinBox::setValue);
    connect(spin_, &QSpinBox::valueChanged, slider_, &QSlider::setValue);
  }

  int value() const { return spin_->value(); }
  void set_value(int value) { spin_->setValue(value); }

private:
  QSlider *slider_ = nullptr;
  QSpinBox *spin_ = nullptr;
};

// Nine squares laid out like the places they stand for.
class AnchorGrid : public QWidget {
public:
  AnchorGrid() {
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(3);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    for (int i = 0; i < 9; ++i) {
      auto *square = new QToolButton();
      const std::string name = "FallbackAnchor" + std::to_string(i);
      square->setCheckable(true);
      square->setFixedSize(30, 30);
      square->setToolTip(text(name.c_str()));
      connect(square, &QToolButton::toggled, square, [square](bool chosen) {
        square->setText(chosen ? QString(QChar(0x25CF)) : QString());
      });
      group_->addButton(square, i);
      grid->addWidget(square, i / 3, i % 3);
    }
    grid->setColumnStretch(3, 1);
  }

  int value() const { return group_->checkedId(); }

  void set_value(int anchor) {
    if (QAbstractButton *square = group_->button(anchor))
      square->setChecked(true);
  }

private:
  QButtonGroup *group_ = nullptr;
};

class FallbackPage : public QWidget {
public:
  FallbackPage(const char *prefix, const char *legacy_key,
               const std::vector<std::string> &scene_sources);

  void load(obs_data_t *settings);
  void save(obs_data_t *changes) const;

private:
  std::string key(const char *field) const { return fallback_key(prefix_, field); }
  QComboBox *animation_list(bool arriving);
  void refresh();

  const char *prefix_;
  const char *legacy_key_;

  QComboBox *kind_ = nullptr;

  QGroupBox *content_box_ = nullptr;
  QFormLayout *content_ = nullptr;
  QComboBox *page_ = nullptr;
  SliderRow *scale_ = nullptr;
  QPlainTextEdit *text_ = nullptr;
  ColorButton *text_color_ = nullptr;
  FontButton *font_ = nullptr;
  QCheckBox *outline_ = nullptr;
  SliderRow *outline_size_ = nullptr;
  ColorButton *outline_color_ = nullptr;
  PathRow *file_ = nullptr;
  QLineEdit *url_ = nullptr;
  QComboBox *scene_source_ = nullptr;

  QGroupBox *placement_box_ = nullptr;
  QFormLayout *placement_ = nullptr;
  QComboBox *fit_ = nullptr;
  SliderRow *size_ = nullptr;
  AnchorGrid *anchor_ = nullptr;
  QCheckBox *keep_ = nullptr;

  QGroupBox *background_box_ = nullptr;
  ColorButton *background_color_ = nullptr;
  PathRow *background_file_ = nullptr;

  QGroupBox *animation_box_ = nullptr;
  QComboBox *anim_in_ = nullptr;
  QComboBox *anim_out_ = nullptr;
  SliderRow *anim_in_ms_ = nullptr;
  SliderRow *anim_out_ms_ = nullptr;
};

static QFormLayout *make_form(QWidget *owner) {
  auto *form = new QFormLayout(owner);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  return form;
}

QComboBox *FallbackPage::animation_list(bool arriving) {
  auto *list = new QComboBox();
  const std::pair<const char *, FallbackAnim> items[] = {
      {"FallbackAnimNone", FallbackAnim::None},
      {"FallbackAnimFade", FallbackAnim::Fade},
      {arriving ? "FallbackAnimFromBottom" : "FallbackAnimToBottom", FallbackAnim::SideBottom},
      {arriving ? "FallbackAnimFromTop" : "FallbackAnimToTop", FallbackAnim::SideTop},
      {arriving ? "FallbackAnimFromRight" : "FallbackAnimToRight", FallbackAnim::SideRight},
      {arriving ? "FallbackAnimFromLeft" : "FallbackAnimToLeft", FallbackAnim::SideLeft},
      {"FallbackAnimZoom", FallbackAnim::Zoom},
      {"FallbackAnimPop", FallbackAnim::Pop},
  };
  for (const auto &item : items)
    list->addItem(text(item.first), static_cast<int>(item.second));
  return list;
}

FallbackPage::FallbackPage(const char *prefix, const char *legacy_key,
                           const std::vector<std::string> &scene_sources)
    : prefix_(prefix), legacy_key_(legacy_key) {
  auto *page = new QVBoxLayout(this);

  kind_ = new QComboBox();
  const std::pair<const char *, FallbackKind> kinds[] = {
      {"FallbackKindNone", FallbackKind::None},   {"FallbackKindCard", FallbackKind::Card},
      {"FallbackKindImage", FallbackKind::Image}, {"FallbackKindVideo", FallbackKind::Video},
      {"FallbackKindBrowser", FallbackKind::Browser},
      {"FallbackKindScene", FallbackKind::SceneSource},
  };
  for (const auto &item : kinds)
    kind_->addItem(text(item.first), static_cast<int>(item.second));
  auto *top = make_form(nullptr);
  top->addRow(text("FallbackKind"), kind_);
  page->addLayout(top);

  content_box_ = new QGroupBox(text("FallbackDialogContent"));
  content_ = make_form(content_box_);

  page_ = new QComboBox();
  page_->addItem(text("FallbackTemplateCustom"), QString());
  for (const auto &entry : FALLBACK_TEMPLATES) {
    if (std::strcmp(entry.prefix, prefix) == 0)
      page_->addItem(text(entry.label), QString::fromUtf8(entry.file));
  }
  content_->addRow(text("FallbackTemplate"), page_);

  scale_ = new SliderRow(50, 300, 5, "%");
  content_->addRow(text("FallbackTemplateScale"), scale_);

  text_ = new QPlainTextEdit();
  text_->setTabChangesFocus(true);
  text_->setFixedHeight(64);
  content_->addRow(text("FallbackText"), text_);

  font_ = new FontButton(text("FallbackFont"));
  content_->addRow(text("FallbackFont"), font_);

  text_color_ = new ColorButton(text("FallbackTextColor"));
  content_->addRow(text("FallbackTextColor"), text_color_);

  outline_ = new QCheckBox(text("FallbackOutline"));
  content_->addRow(QString(), outline_);

  outline_size_ = new SliderRow(1, 20, 1, QString());
  content_->addRow(text("FallbackOutlineSize"), outline_size_);

  outline_color_ = new ColorButton(text("FallbackOutlineColor"));
  content_->addRow(text("FallbackOutlineColor"), outline_color_);

  file_ = new PathRow(text("FallbackFile"));
  content_->addRow(text("FallbackFile"), file_);

  url_ = new QLineEdit();
  content_->addRow(text("FallbackUrl"), url_);

  scene_source_ = new QComboBox();
  scene_source_->addItem(text("FallbackKindNone"), QString());
  for (const std::string &name : scene_sources)
    scene_source_->addItem(QString::fromStdString(name), QString::fromStdString(name));
  content_->addRow(text("FallbackSceneSource"), scene_source_);

  placement_box_ = new QGroupBox(text("FallbackDialogPlacement"));
  placement_ = make_form(placement_box_);

  fit_ = new QComboBox();
  fit_->addItem(text("FallbackFitFit"), static_cast<int>(FallbackFit::Fit));
  fit_->addItem(text("FallbackFitFill"), static_cast<int>(FallbackFit::Fill));
  fit_->addItem(text("FallbackFitStretch"), static_cast<int>(FallbackFit::Stretch));
  fit_->addItem(text("FallbackFitOriginal"), static_cast<int>(FallbackFit::Original));
  placement_->addRow(text("FallbackFit"), fit_);

  size_ = new SliderRow(5, 100, 1, "%");
  placement_->addRow(text("FallbackSize"), size_);

  anchor_ = new AnchorGrid();
  placement_->addRow(text("FallbackAnchor"), anchor_);

  keep_ = new QCheckBox(text("FallbackKeep"));
  placement_->addRow(QString(), keep_);

  background_box_ = new QGroupBox(text("FallbackDialogBackground"));
  auto *background = make_form(background_box_);
  background_color_ = new ColorButton(text("FallbackBackground"));
  background->addRow(text("FallbackBackground"), background_color_);
  background_file_ = new PathRow(text("FallbackBackgroundFile"));
  background_file_->set_filter(IMAGE_FILTER);
  background->addRow(text("FallbackBackgroundFile"), background_file_);

  animation_box_ = new QGroupBox(text("FallbackDialogAnimation"));
  auto *animation = make_form(animation_box_);
  anim_in_ = animation_list(true);
  anim_in_ms_ = new SliderRow(0, 3000, 50, " ms");
  anim_out_ = animation_list(false);
  anim_out_ms_ = new SliderRow(0, 3000, 50, " ms");
  animation->addRow(text("FallbackAnimIn"), anim_in_);
  animation->addRow(text("FallbackAnimInMs"), anim_in_ms_);
  animation->addRow(text("FallbackAnimOut"), anim_out_);
  animation->addRow(text("FallbackAnimOutMs"), anim_out_ms_);

  auto *left = new QVBoxLayout();
  left->addWidget(content_box_);
  left->addWidget(placement_box_);
  left->addStretch(1);

  auto *right = new QVBoxLayout();
  right->addWidget(background_box_);
  right->addWidget(animation_box_);
  right->addStretch(1);

  auto *columns = new QHBoxLayout();
  columns->addLayout(left, 1);
  columns->addLayout(right, 1);
  page->addLayout(columns, 1);

  const auto on_choice = [this](int) { refresh(); };
  connect(kind_, &QComboBox::currentIndexChanged, this, on_choice);
  connect(page_, &QComboBox::currentIndexChanged, this, on_choice);
  connect(outline_, &QCheckBox::toggled, this, [this](bool) { refresh(); });
}

static void select(QComboBox *list, const QVariant &value) {
  const int index = list->findData(value);
  list->setCurrentIndex(index >= 0 ? index : 0);
}

void FallbackPage::load(obs_data_t *settings) {
  const auto string_of = [&](const char *field) { return obs_data_get_string(settings, key(field).c_str()); };
  const auto number_of = [&](const char *field) {
    return static_cast<int>(obs_data_get_int(settings, key(field).c_str()));
  };
  const auto color_of = [&](const char *field) {
    return static_cast<uint32_t>(obs_data_get_int(settings, key(field).c_str()));
  };

  select(kind_, number_of("kind"));
  select(page_, QString::fromUtf8(string_of("template")));
  scale_->set_value(number_of("scale"));
  text_->setPlainText(QString::fromUtf8(string_of("text")));
  text_color_->set_value(color_of("text_color"));
  obs_data_t *font = obs_data_get_obj(settings, key("font").c_str());
  font_->load(font);
  obs_data_release(font);
  outline_->setChecked(obs_data_get_bool(settings, key("outline").c_str()));
  outline_size_->set_value(number_of("outline_size"));
  outline_color_->set_value(color_of("outline_color"));
  file_->set_value(string_of("file"));
  url_->setText(QString::fromUtf8(string_of("url")));

  // A source can have gone from the scene since it was picked.
  const QString scene_source = QString::fromUtf8(obs_data_get_string(settings, legacy_key_));
  if (!scene_source.isEmpty() && scene_source_->findData(scene_source) < 0)
    scene_source_->addItem(scene_source, scene_source);
  select(scene_source_, scene_source);

  select(fit_, number_of("fit"));
  size_->set_value(number_of("size"));
  anchor_->set_value(number_of("anchor"));
  keep_->setChecked(obs_data_get_bool(settings, key("keep").c_str()));
  background_color_->set_value(color_of("background_color"));
  background_file_->set_value(string_of("background_file"));
  select(anim_in_, number_of("anim_in"));
  anim_in_ms_->set_value(number_of("anim_in_ms"));
  select(anim_out_, number_of("anim_out"));
  anim_out_ms_->set_value(number_of("anim_out_ms"));

  refresh();
}

void FallbackPage::save(obs_data_t *changes) const {
  const auto set_string = [&](const char *field, const std::string &value) {
    obs_data_set_string(changes, key(field).c_str(), value.c_str());
  };
  const auto set_number = [&](const char *field, int64_t value) {
    obs_data_set_int(changes, key(field).c_str(), value);
  };

  set_number("kind", kind_->currentData().toInt());
  set_string("template", page_->currentData().toString().toUtf8().constData());
  set_number("scale", scale_->value());
  set_string("text", text_->toPlainText().toUtf8().constData());
  set_number("text_color", text_color_->value());
  obs_data_t *font = font_->save();
  obs_data_set_obj(changes, key("font").c_str(), font);
  obs_data_release(font);
  obs_data_set_bool(changes, key("outline").c_str(), outline_->isChecked());
  set_number("outline_size", outline_size_->value());
  set_number("outline_color", outline_color_->value());
  set_string("file", file_->value());
  set_string("url", url_->text().toUtf8().constData());
  obs_data_set_string(changes, legacy_key_, scene_source_->currentData().toString().toUtf8().constData());
  set_number("fit", fit_->currentData().toInt());
  set_number("size", size_->value());
  set_number("anchor", anchor_->value());
  obs_data_set_bool(changes, key("keep").c_str(), keep_->isChecked());
  set_number("background_color", background_color_->value());
  set_string("background_file", background_file_->value());
  set_number("anim_in", anim_in_->currentData().toInt());
  set_number("anim_in_ms", anim_in_ms_->value());
  set_number("anim_out", anim_out_->currentData().toInt());
  set_number("anim_out_ms", anim_out_ms_->value());
}

// Only what applies to the current choice is on show.
void FallbackPage::refresh() {
  const auto kind = static_cast<FallbackKind>(kind_->currentData().toInt());
  const bool card = kind == FallbackKind::Card;
  const bool image = kind == FallbackKind::Image;
  const bool video = kind == FallbackKind::Video;
  const bool browser = kind == FallbackKind::Browser;
  const bool scene = kind == FallbackKind::SceneSource;
  const bool draws = kind != FallbackKind::None && !scene;
  const bool shipped_page = browser && !page_->currentData().toString().isEmpty();
  const bool custom_page = browser && !shipped_page;
  const bool outline = outline_->isChecked();

  content_box_->setVisible(draws || scene);
  content_->setRowVisible(page_, browser);
  content_->setRowVisible(scale_, shipped_page);
  content_->setRowVisible(text_, card || shipped_page);
  content_->setRowVisible(font_, card);
  content_->setRowVisible(text_color_, card || shipped_page);
  content_->setRowVisible(outline_, card);
  content_->setRowVisible(outline_size_, card && outline);
  content_->setRowVisible(outline_color_, card && outline);
  content_->setRowVisible(file_, image || video || custom_page);
  content_->setRowVisible(url_, custom_page);
  content_->setRowVisible(scene_source_, scene);
  file_->set_filter(image ? IMAGE_FILTER : video ? VIDEO_FILTER : PAGE_FILTER);

  placement_box_->setVisible(draws);
  placement_->setRowVisible(fit_, draws && !card && !shipped_page);
  placement_->setRowVisible(size_, !shipped_page);
  placement_->setRowVisible(anchor_, !shipped_page);
  background_box_->setVisible(draws);
  animation_box_->setVisible(draws);
}

FallbackDialog::FallbackDialog(obs_data_t *settings, const std::vector<std::string> &scene_sources,
                               FallbackApply apply, QWidget *parent)
    : QDialog(parent), apply_(std::move(apply)) {
  setWindowTitle(text("FallbackDialogTitle"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setMinimumSize(760, 520);
  resize(980, 680);

  struct Section {
    const char *title;
    const char *prefix;
    const char *legacy_key;
  };
  const Section sections[] = {
      {"FallbackDisconnect", FALLBACK_DISCONNECT, PROP_DISCONNECT_SOURCE},
      {"FallbackLow", FALLBACK_LOW, PROP_LOW_BITRATE_SOURCE},
      {"FallbackLoading", FALLBACK_LOADING, PROP_LOADING_SOURCE},
  };

  auto *tabs = new QTabWidget();
  for (const Section &section : sections) {
    auto *page = new FallbackPage(section.prefix, section.legacy_key, scene_sources);
    page->load(settings);
    pages_.push_back(page);

    // A small screen scrolls instead of cutting fields off.
    auto *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    tabs->addTab(scroll, text(section.title));
  }

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
  buttons->button(QDialogButtonBox::Ok)->setText(text("FallbackDialogOk"));
  buttons->button(QDialogButtonBox::Cancel)->setText(text("FallbackDialogCancel"));
  buttons->button(QDialogButtonBox::Apply)->setText(text("FallbackDialogApply"));
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    apply_changes();
    accept();
  });
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
          &FallbackDialog::apply_changes);

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(tabs, 1);
  layout->addWidget(buttons);
}

void FallbackDialog::apply_changes() {
  obs_data_t *changes = obs_data_create();
  for (const FallbackPage *page : pages_)
    page->save(changes);
  apply_(changes);
  obs_data_release(changes);
}

void open_fallback_dialog(obs_data_t *settings, const std::vector<std::string> &scene_sources,
                          FallbackApply apply) {
  FallbackDialog dialog(settings, scene_sources, std::move(apply), QApplication::activeWindow());
  dialog.exec();
}

} // namespace lss
