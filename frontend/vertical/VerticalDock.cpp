/******************************************************************************
    Siegfried: Vertical canvas dock (preview, scenes, sources).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "VerticalDock.hpp"
#include "VerticalCanvas.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <Idian/Utils.hpp>
#include <utility/display-helpers.hpp>
#include <qt-wrappers.hpp>

#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "moc_VerticalDock.cpp"

namespace {

/* Colours are 0xAABBGGRR (vec4_from_rgba) */
constexpr uint32_t kSelectColor = 0xFF4DA33F;   /* Siegfried green */
constexpr uint32_t kSafeZoneColor = 0x400000FF; /* translucent red */
constexpr uint32_t kSafeLineColor = 0xA00000FF;
constexpr float kHandlePx = 10.0f;

/* Phone app overlays (TikTok / Shorts / Reels): title bar on top, caption
 * and buttons at the bottom, like/comment/share column on the right. */
constexpr float kSafeTop = 0.10f;
constexpr float kSafeBottom = 0.20f;
constexpr float kSafeRight = 0.14f;

void FillRect(float x, float y, float w, float h, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_from_rgba(&c, color);
	gs_effect_set_vec4(param, &c);

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 1, 1);
	}
	gs_matrix_pop();
}

void DrawLine(const vec3 &a, const vec3 &b, float thickness, uint32_t color)
{
	float dx = b.x - a.x, dy = b.y - a.y;
	float len = std::sqrt(dx * dx + dy * dy);
	if (len <= 0.0f) {
		return;
	}
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	vec4 c;
	vec4_from_rgba(&c, color);
	gs_effect_set_vec4(param, &c);

	gs_matrix_push();
	gs_matrix_translate3f(a.x, a.y, 0.0f);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, std::atan2(dy, dx));
	gs_matrix_translate3f(0.0f, -thickness / 2.0f, 0.0f);
	gs_matrix_scale3f(len, thickness, 1.0f);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 1, 1);
	}
	gs_matrix_pop();
}

void BoxCorners(obs_sceneitem_t *item, vec3 out[4])
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	const float pts[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (int i = 0; i < 4; i++) {
		vec3 p;
		vec3_set(&p, pts[i][0], pts[i][1], 0.0f);
		vec3_transform(&out[i], &p, &m);
	}
}

vec2 BoxCenter(obs_sceneitem_t *item)
{
	vec3 c[4];
	BoxCorners(item, c);
	vec2 r;
	vec2_set(&r, (c[0].x + c[2].x) / 2.0f, (c[0].y + c[2].y) / 2.0f);
	return r;
}

bool PointInItem(obs_sceneitem_t *item, const vec2 &pos)
{
	matrix4 m, inv;
	obs_sceneitem_get_box_transform(item, &m);
	matrix4_inv(&inv, &m);
	vec3 p, local;
	vec3_set(&p, pos.x, pos.y, 0.0f);
	vec3_transform(&local, &p, &inv);
	return local.x >= 0.0f && local.x <= 1.0f && local.y >= 0.0f && local.y <= 1.0f;
}

OBSScene SceneOf(VerticalCanvas *vc)
{
	OBSSource s = vc->CurrentScene();
	return obs_scene_from_source(s);
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Preview                                                                  */

VerticalPreview::VerticalPreview(QWidget *parent, VerticalCanvas *vc_) : OBSQTDisplay(parent), vc(vc_)
{
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(false);
	setMinimumSize(120, 200);

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(GetDisplay(), Render, this);
	};
	connect(this, &OBSQTDisplay::DisplayCreated, this, addDrawCallback);
}

VerticalPreview::~VerticalPreview()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), Render, this);
	}
}

void VerticalPreview::Render(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<VerticalPreview *>(data);
	OBSCanvasAutoRelease canvas = self->vc->GetCanvas();
	uint32_t bw = 0, bh = 0;
	if (!canvas || !obs_canvas_get_base_size(canvas, &bw, &bh) || !cx || !cy) {
		return;
	}

	int x, y;
	float scale;
	GetScaleAndCenterPos(int(bw), int(bh), int(cx), int(cy), x, y, scale);
	int vw = int(scale * float(bw));
	int vh = int(scale * float(bh));

	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, vw, vh);
	gs_ortho(0.0f, float(bw), 0.0f, float(bh), -100.0f, 100.0f);

	FillRect(0.0f, 0.0f, float(bw), float(bh), 0xFF000000);
	obs_canvas_render(canvas);
	gs_load_vertexbuffer(nullptr);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	float px = 1.0f / std::max(scale, 0.0001f); /* one screen pixel in canvas units */

	if (self->vc->SafeZones()) {
		float top = float(bh) * kSafeTop;
		float bottom = float(bh) * kSafeBottom;
		float right = float(bw) * kSafeRight;
		FillRect(0.0f, 0.0f, float(bw), top, kSafeZoneColor);
		FillRect(0.0f, float(bh) - bottom, float(bw), bottom, kSafeZoneColor);
		FillRect(float(bw) - right, top, right, float(bh) - top - bottom, kSafeZoneColor);

		vec3 a, b;
		vec3_set(&a, 0.0f, top, 0.0f);
		vec3_set(&b, float(bw) - right, top, 0.0f);
		DrawLine(a, b, px, kSafeLineColor);
		vec3_set(&a, 0.0f, float(bh) - bottom, 0.0f);
		vec3_set(&b, float(bw) - right, float(bh) - bottom, 0.0f);
		DrawLine(a, b, px, kSafeLineColor);
		vec3_set(&a, float(bw) - right, top, 0.0f);
		vec3_set(&b, float(bw) - right, float(bh) - bottom, 0.0f);
		DrawLine(a, b, px, kSafeLineColor);
	}

	/* Selection outline and corner handles */
	OBSSourceAutoRelease sceneSource = obs_canvas_get_channel(canvas, 0);
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		struct Ctx {
			float px;
		} ctx{px};
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (!obs_sceneitem_selected(item) || !obs_sceneitem_visible(item)) {
				return true;
			}
			float p = static_cast<Ctx *>(param)->px;
			vec3 c[4];
			BoxCorners(item, c);
			for (int i = 0; i < 4; i++) {
				DrawLine(c[i], c[(i + 1) % 4], 2.0f * p, kSelectColor);
			}
			float hs = kHandlePx * p;
			for (int i = 0; i < 4; i++) {
				FillRect(c[i].x - hs / 2.0f, c[i].y - hs / 2.0f, hs, hs, kSelectColor);
			}
			return true;
		};
		obs_scene_enum_items(scene, cb, &ctx);
	}

	gs_blend_state_pop();
	gs_viewport_pop();
	gs_projection_pop();
}

bool VerticalPreview::MapToCanvas(const QPointF &widgetPos, vec2 &out, float *scaleOut) const
{
	uint32_t bw, bh;
	if (!vc->GetSize(bw, bh)) {
		return false;
	}
	QSize px = GetPixelSize(const_cast<VerticalPreview *>(this));
	if (px.isEmpty()) {
		return false;
	}
	int x, y;
	float scale;
	GetScaleAndCenterPos(int(bw), int(bh), px.width(), px.height(), x, y, scale);
	qreal dpr = devicePixelRatioF();
	vec2_set(&out, float((widgetPos.x() * dpr - x) / scale), float((widgetPos.y() * dpr - y) / scale));
	if (scaleOut) {
		*scaleOut = float(scale / dpr); /* canvas units -> widget units */
	}
	return true;
}

OBSSceneItem VerticalPreview::ItemAt(const vec2 &pos) const
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return nullptr;
	}
	struct Ctx {
		vec2 pos;
		OBSSceneItem hit;
	} ctx{pos, nullptr};
	/* Enumerates bottom to top, so the last hit is the topmost */
	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *c = static_cast<Ctx *>(param);
		if (obs_sceneitem_visible(item) && !obs_sceneitem_locked(item) && PointInItem(item, c->pos)) {
			c->hit = item;
		}
		return true;
	};
	obs_scene_enum_items(scene, cb, &ctx);
	return ctx.hit;
}

OBSSceneItem VerticalPreview::HandleAt(const vec2 &pos, float scale) const
{
	OBSSceneItem item = Selected();
	if (!item || !obs_sceneitem_visible(item)) {
		return nullptr;
	}
	float radius = kHandlePx / std::max(scale, 0.0001f);
	vec3 c[4];
	BoxCorners(item, c);
	for (const vec3 &p : c) {
		if (std::fabs(pos.x - p.x) <= radius && std::fabs(pos.y - p.y) <= radius) {
			return item;
		}
	}
	return nullptr;
}

OBSSceneItem VerticalPreview::Selected() const
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return nullptr;
	}
	OBSSceneItem found;
	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		if (obs_sceneitem_selected(item)) {
			*static_cast<OBSSceneItem *>(param) = item;
		}
		return true;
	};
	obs_scene_enum_items(scene, cb, &found);
	return found;
}

void VerticalPreview::Select(obs_sceneitem_t *target)
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return;
	}
	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		bool want = item == static_cast<obs_sceneitem_t *>(param);
		if (obs_sceneitem_selected(item) != want) {
			obs_sceneitem_select(item, want);
		}
		return true;
	};
	obs_scene_enum_items(scene, cb, target);
	emit SelectionChanged();
}

void VerticalPreview::Snap(obs_sceneitem_t *item, float tol)
{
	uint32_t bw, bh;
	if (!vc->GetSize(bw, bh)) {
		return;
	}
	vec3 c[4];
	BoxCorners(item, c);
	float minX = c[0].x, maxX = c[0].x, minY = c[0].y, maxY = c[0].y;
	for (const vec3 &p : c) {
		minX = std::min(minX, p.x);
		maxX = std::max(maxX, p.x);
		minY = std::min(minY, p.y);
		maxY = std::max(maxY, p.y);
	}

	auto best = [tol](std::initializer_list<float> diffs) {
		float pick = 0.0f, bestAbs = tol;
		for (float d : diffs) {
			if (std::fabs(d) < bestAbs) {
				bestAbs = std::fabs(d);
				pick = d;
			}
		}
		return pick;
	};
	float dx = best({-minX, float(bw) - maxX, float(bw) / 2.0f - (minX + maxX) / 2.0f});
	float dy = best({-minY, float(bh) - maxY, float(bh) / 2.0f - (minY + maxY) / 2.0f});
	if (dx != 0.0f || dy != 0.0f) {
		vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		pos.x += dx;
		pos.y += dy;
		obs_sceneitem_set_pos(item, &pos);
	}
}

void VerticalPreview::ScaleAroundCenter(obs_sceneitem_t *item, float factor)
{
	vec2 c0 = BoxCenter(item);
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		vec2 b;
		obs_sceneitem_get_bounds(item, &b);
		vec2_mulf(&b, &b, factor);
		obs_sceneitem_set_bounds(item, &b);
	} else {
		vec2 s;
		obs_sceneitem_get_scale(item, &s);
		vec2_mulf(&s, &s, factor);
		obs_sceneitem_set_scale(item, &s);
	}
	vec2 c1 = BoxCenter(item);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += c0.x - c1.x;
	pos.y += c0.y - c1.y;
	obs_sceneitem_set_pos(item, &pos);
}

void VerticalPreview::mousePressEvent(QMouseEvent *event)
{
	OBSQTDisplay::mousePressEvent(event);
	emit Activated();
	if (event->button() != Qt::LeftButton) {
		return;
	}
	vec2 pos;
	float scale;
	if (!MapToCanvas(event->position(), pos, &scale)) {
		return;
	}

	dragStart = pos;
	if (OBSSceneItem item = HandleAt(pos, scale)) {
		mode = Mode::Scale;
		dragItem = item;
		center = BoxCenter(item);
		float dx = pos.x - center.x, dy = pos.y - center.y;
		startDistance = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
		obs_sceneitem_get_scale(item, &itemStartScale);
		obs_sceneitem_get_bounds(item, &itemStartBounds);
		obs_sceneitem_get_pos(item, &itemStartPos);
		return;
	}

	OBSSceneItem item = ItemAt(pos);
	Select(item);
	if (item) {
		mode = Mode::Move;
		dragItem = item;
		obs_sceneitem_get_pos(item, &itemStartPos);
	} else {
		mode = Mode::None;
		dragItem = nullptr;
	}
}

void VerticalPreview::mouseMoveEvent(QMouseEvent *event)
{
	OBSQTDisplay::mouseMoveEvent(event);
	if (mode == Mode::None || !dragItem) {
		return;
	}
	vec2 pos;
	float scale;
	if (!MapToCanvas(event->position(), pos, &scale)) {
		return;
	}
	bool snap = !(event->modifiers() & Qt::ControlModifier);

	if (mode == Mode::Move) {
		vec2 p;
		vec2_set(&p, itemStartPos.x + pos.x - dragStart.x, itemStartPos.y + pos.y - dragStart.y);
		obs_sceneitem_set_pos(dragItem, &p);
		if (snap) {
			Snap(dragItem, 12.0f / std::max(scale, 0.0001f));
		}
		return;
	}

	/* Scale: uniform, around the item's centre */
	float dx = pos.x - center.x, dy = pos.y - center.y;
	float factor = std::max(0.02f, std::sqrt(dx * dx + dy * dy) / startDistance);
	obs_sceneitem_set_pos(dragItem, &itemStartPos);
	if (obs_sceneitem_get_bounds_type(dragItem) != OBS_BOUNDS_NONE) {
		obs_sceneitem_set_bounds(dragItem, &itemStartBounds);
	} else {
		obs_sceneitem_set_scale(dragItem, &itemStartScale);
	}
	ScaleAroundCenter(dragItem, factor);
}

void VerticalPreview::mouseReleaseEvent(QMouseEvent *event)
{
	OBSQTDisplay::mouseReleaseEvent(event);
	if (event->button() == Qt::LeftButton) {
		mode = Mode::None;
		dragItem = nullptr;
	}
}

void VerticalPreview::wheelEvent(QWheelEvent *event)
{
	OBSSceneItem item = Selected();
	if (!item) {
		OBSQTDisplay::wheelEvent(event);
		return;
	}
	float steps = float(event->angleDelta().y()) / 120.0f;
	if (steps != 0.0f) {
		ScaleAroundCenter(item, std::pow(1.05f, steps));
	}
	event->accept();
}

void VerticalPreview::keyPressEvent(QKeyEvent *event)
{
	OBSSceneItem item = Selected();
	if (!item) {
		OBSQTDisplay::keyPressEvent(event);
		return;
	}
	float step = (event->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	switch (event->key()) {
	case Qt::Key_Left:
		pos.x -= step;
		break;
	case Qt::Key_Right:
		pos.x += step;
		break;
	case Qt::Key_Up:
		pos.y -= step;
		break;
	case Qt::Key_Down:
		pos.y += step;
		break;
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		obs_sceneitem_remove(item);
		emit SelectionChanged();
		return;
	default:
		OBSQTDisplay::keyPressEvent(event);
		return;
	}
	obs_sceneitem_set_pos(item, &pos);
}

void VerticalPreview::contextMenuEvent(QContextMenuEvent *event)
{
	vec2 pos;
	if (!MapToCanvas(event->pos(), pos)) {
		return;
	}
	OBSSceneItem item = ItemAt(pos);
	if (!item) {
		item = Selected();
	}
	if (!item) {
		return;
	}
	Select(item);

	QMenu menu(this);
	auto fit = [&](const QString &label, VerticalCanvas::Fit mode) {
		menu.addAction(label, this, [item, mode] { VerticalCanvas::FitItem(item, mode); });
	};
	fit(QTStr("Vertical.Fit.Width"), VerticalCanvas::Fit::Width);
	fit(QTStr("Vertical.Fit.Inside"), VerticalCanvas::Fit::Inside);
	fit(QTStr("Vertical.Fit.Fill"), VerticalCanvas::Fit::Fill);
	fit(QTStr("Vertical.Fit.Center"), VerticalCanvas::Fit::Center);
	fit(QTStr("Vertical.Fit.Reset"), VerticalCanvas::Fit::Reset);
	menu.addSeparator();
	menu.addAction(QTStr("Vertical.Order.Front"), this,
		       [item] { obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP); });
	menu.addAction(QTStr("Vertical.Order.Back"), this,
		       [item] { obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM); });
	menu.addSeparator();
	menu.addAction(QTStr("Vertical.Hide"), this, [item] { obs_sceneitem_set_visible(item, false); });
	menu.addAction(QTStr("Vertical.RemoveSource"), this, [this, item] {
		obs_sceneitem_remove(item);
		emit SelectionChanged();
	});
	menu.exec(event->globalPos());
}

/* ------------------------------------------------------------------------ */
/* Pane beside the main preview                                             */

VerticalPane::VerticalPane(QWidget *parent, VerticalCanvas *vc_) : QWidget(parent), vc(vc_)
{
	title = new QLabel(this);
	title->setStyleSheet("color: palette(placeholder-text);");
	title->setAlignment(Qt::AlignCenter);

	preview = new VerticalPreview(this, vc);
	preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	preview->setMinimumWidth(90);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);
	layout->addWidget(preview, 1);
	layout->addWidget(title);

	connect(vc, &VerticalCanvas::CanvasChanged, this, &VerticalPane::UpdateTitle);
	connect(vc, &VerticalCanvas::CurrentSceneChanged, this, &VerticalPane::UpdateTitle);
	connect(vc, &VerticalCanvas::ScenesChanged, this, &VerticalPane::UpdateTitle);
	connect(vc, &VerticalCanvas::SettingsChanged, this, &VerticalPane::UpdateTitle);
	UpdateTitle();
}

void VerticalPane::UpdateTitle()
{
	uint32_t cx = 0, cy = 0;
	QString text = QTStr("Vertical.PaneTitle");
	if (vc->GetSize(cx, cy)) {
		text += QStringLiteral(" · %1×%2").arg(cx).arg(cy);
	}
	if (OBSSource s = vc->CurrentScene()) {
		text += QStringLiteral(" · ") + QString::fromUtf8(obs_source_get_name(s));
	}
	title->setText(text);
}

/* ------------------------------------------------------------------------ */
/* Shared toolbar helpers: same look as the main Scenes/Sources toolbars    */

static QAction *ToolAction(QToolBar *bar, const char *iconClass, const char *icon, const QString &tip)
{
	QAction *a = new QAction(bar);
	a->setIcon(QIcon(QString::fromUtf8(icon)));
	a->setToolTip(tip);
	a->setText(tip);
	a->setProperty("class", QString::fromUtf8(iconClass));
	bar->addAction(a);
	return a;
}

static QToolBar *MakeToolbar(QWidget *parent)
{
	QToolBar *bar = new QToolBar(parent);
	bar->setIconSize(QSize(16, 16));
	bar->setFloatable(false);
	bar->setMovable(false);
	return bar;
}

static void FinishToolbar(QToolBar *bar)
{
	/* Themes style the buttons through the actions' dynamic properties */
	for (QAction *a : bar->actions()) {
		if (QWidget *w = bar->widgetForAction(a)) {
			for (const QByteArray &name : a->dynamicPropertyNames()) {
				w->setProperty(name.constData(), a->property(name.constData()));
			}
		}
	}
	RefreshToolBarStyling(bar);
}

static QListWidget *MakeList(QWidget *parent)
{
	QListWidget *list = new QListWidget(parent);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setFrameShape(QFrame::NoFrame);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	return list;
}

static QFrame *MakeFrame(QWidget *parent, QListWidget *list, QToolBar *bar)
{
	QFrame *frame = new QFrame(parent);
	QVBoxLayout *inner = new QVBoxLayout(frame);
	inner->setContentsMargins(0, 0, 0, 0);
	inner->setSpacing(0);
	inner->addWidget(list, 1);
	inner->addWidget(bar);

	QVBoxLayout *outer = new QVBoxLayout(parent);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->addWidget(frame);
	return frame;
}

/* ------------------------------------------------------------------------ */
/* Scenes panel                                                             */

VerticalScenesPanel::VerticalScenesPanel(QWidget *parent, VerticalCanvas *vc_) : QWidget(parent), vc(vc_)
{
	list = MakeList(this);
	list->setMouseTracking(true);
	toolbar = MakeToolbar(this);
	QAction *add = ToolAction(toolbar, "icon-plus", ":/res/images/plus.svg", QTStr("Vertical.AddScene"));
	QAction *remove = ToolAction(toolbar, "icon-trash", ":/res/images/minus.svg", QTStr("Vertical.RemoveScene"));
	toolbar->addSeparator();
	optionsAction = ToolAction(toolbar, "icon-dots-vert", ":/res/images/dots-vert.svg", QTStr("Vertical.Options"));
	MakeFrame(this, list, toolbar);
	FinishToolbar(toolbar);

	connect(add, &QAction::triggered, this, &VerticalScenesPanel::AddScene);
	connect(remove, &QAction::triggered, this, &VerticalScenesPanel::RemoveScene);
	connect(optionsAction, &QAction::triggered, this, &VerticalScenesPanel::ShowOptionsMenu);

	connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *li) {
		if (updating || !li) {
			return;
		}
		OBSSource s = li->data(Qt::UserRole).value<OBSSource>();
		if (s) {
			vc->SetCurrentScene(s);
		}
	});
	connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { RenameScene(); });
	connect(list, &QListWidget::customContextMenuRequested, this, &VerticalScenesPanel::ShowContextMenu);

	connect(vc, &VerticalCanvas::ScenesChanged, this, &VerticalScenesPanel::Refresh);
	connect(vc, &VerticalCanvas::CurrentSceneChanged, this, &VerticalScenesPanel::Refresh);
	connect(vc, &VerticalCanvas::CanvasChanged, this, &VerticalScenesPanel::Refresh);
	connect(vc, &VerticalCanvas::SettingsChanged, this, &VerticalScenesPanel::Refresh);
	connect(vc, &VerticalCanvas::MainSceneChanged, this, &VerticalScenesPanel::UpdateTooltips);
	Refresh();
}

void VerticalScenesPanel::Refresh()
{
	updating = true;
	list->clear();
	OBSSource current = vc->CurrentScene();
	for (const OBSSource &s : vc->Scenes()) {
		QListWidgetItem *li = new QListWidgetItem(QString::fromUtf8(obs_source_get_name(s)));
		li->setData(Qt::UserRole, QVariant::fromValue(s));
		list->addItem(li);
		if (s == current) {
			list->setCurrentItem(li);
		}
	}
	setEnabled(vc->GetCanvas() != nullptr);
	updating = false;
	UpdateTooltips();
}

/* Link status on hover only */
QString VerticalScenesPanel::LinkTooltip(obs_source_t *scene) const
{
	QStringList lines;
	QStringList linkedNames;
	for (const QString &u : vc->LinkedMainScenes(scene)) {
		OBSSourceAutoRelease ms = obs_get_source_by_uuid(u.toUtf8().constData());
		if (ms) {
			linkedNames << QString::fromUtf8(obs_source_get_name(ms));
		}
	}
	if (!linkedNames.isEmpty()) {
		lines << QTStr("Vertical.Tip.Linked").arg(linkedNames.join(QStringLiteral(", ")));
	} else {
		lines << QTStr("Vertical.Tip.ByName").arg(QString::fromUtf8(obs_source_get_name(scene)));
	}
	if (!vc->Linked()) {
		lines << QTStr("Vertical.Tip.FollowOff");
	} else if (scene == vc->CurrentScene().Get()) {
		OBSSourceAutoRelease live = obs_frontend_get_current_scene();
		if (live && !vc->SceneForMain(live)) {
			lines << QTStr("Vertical.Tip.NoMatch").arg(QString::fromUtf8(obs_source_get_name(live)),
							   QString::fromUtf8(obs_source_get_name(scene)));
		}
	}
	return lines.join('\n');
}

void VerticalScenesPanel::UpdateTooltips()
{
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *li = list->item(i);
		OBSSource s = li->data(Qt::UserRole).value<OBSSource>();
		if (s) {
			li->setToolTip(LinkTooltip(s));
		}
	}
}

void VerticalScenesPanel::FillSceneMenu(QMenu *menu, OBSSource current)
{
	menu->addAction(QTStr("Vertical.RenameScene"), this, &VerticalScenesPanel::RenameScene);
	menu->addAction(QTStr("Vertical.RemoveScene"), this, &VerticalScenesPanel::RemoveScene);

	/* Pick the main scenes this vertical scene goes with */
	QMenu *links = menu->addMenu(QTStr("Vertical.LinkedScenes"));
	QStringList mine = vc->LinkedMainScenes(current);
	obs_frontend_source_list mainScenes = {};
	obs_frontend_get_scenes(&mainScenes);
	for (size_t i = 0; i < mainScenes.sources.num; i++) {
		obs_source_t *ms = mainScenes.sources.array[i];
		QString uuid = QString::fromUtf8(obs_source_get_uuid(ms));
		QString name = QString::fromUtf8(obs_source_get_name(ms));
		/* Show where else it's linked, so moving a link is obvious */
		OBSSource other = vc->SceneForMain(ms);
		QString label = name;
		if (other && other.Get() != current.Get() && vc->LinkedMainScenes(other).contains(uuid)) {
			label = QTStr("Vertical.LinkedElsewhere").arg(name, QString::fromUtf8(obs_source_get_name(other)));
		}
		QAction *a = links->addAction(label);
		a->setCheckable(true);
		a->setChecked(mine.contains(uuid));
		OBSSource msRef = ms;
		connect(a, &QAction::toggled, this,
			[this, current, msRef](bool on) { vc->SetSceneLink(current, msRef, on); });
	}
	obs_frontend_source_list_free(&mainScenes);
	if (links->isEmpty()) {
		links->setEnabled(false);
	}
}

void VerticalScenesPanel::ShowContextMenu(const QPoint &pos)
{
	QMenu menu(this);
	QListWidgetItem *li = list->itemAt(pos);
	if (li) {
		OBSSource s = li->data(Qt::UserRole).value<OBSSource>();
		if (s) {
			vc->SetCurrentScene(s);
			FillSceneMenu(&menu, s);
			menu.addSeparator();
		}
	}
	menu.addAction(QTStr("Vertical.AddScene"), this, &VerticalScenesPanel::AddScene);
	menu.exec(list->viewport()->mapToGlobal(pos));
}

void VerticalScenesPanel::ShowOptionsMenu()
{
	QMenu menu(this);
	if (OBSSource current = vc->CurrentScene()) {
		FillSceneMenu(&menu, current);
		menu.addSeparator();
	}
	QAction *linked = menu.addAction(QTStr("Vertical.Linked"));
	linked->setCheckable(true);
	linked->setChecked(vc->Linked());
	linked->setToolTip(QTStr("Vertical.LinkedTooltip"));
	connect(linked, &QAction::toggled, vc, &VerticalCanvas::SetLinked);
	QAction *safe = menu.addAction(QTStr("Vertical.SafeZones"));
	safe->setCheckable(true);
	safe->setChecked(vc->SafeZones());
	connect(safe, &QAction::toggled, vc, &VerticalCanvas::SetSafeZones);
	menu.setToolTipsVisible(true);
	QWidget *button = toolbar->widgetForAction(optionsAction);
	menu.exec(button ? button->mapToGlobal(QPoint(0, button->height())) : QCursor::pos());
}

void VerticalScenesPanel::AddScene()
{
	/* Default: same name as the main scene, so they're linked */
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	QString suggestion = vc->UniqueSceneName(mainScene ? QString::fromUtf8(obs_source_get_name(mainScene))
							   : QStringLiteral("Scene"));
	bool ok = false;
	QString name = QInputDialog::getText(this, QTStr("Vertical.AddScene"), QTStr("Vertical.SceneName"),
					     QLineEdit::Normal, suggestion, &ok);
	if (!ok || name.trimmed().isEmpty()) {
		return;
	}
	OBSSource s = vc->CreateScene(name);
	if (s) {
		vc->SetCurrentScene(s);
	}
}

void VerticalScenesPanel::RemoveScene()
{
	OBSSource s = vc->CurrentScene();
	if (!s) {
		return;
	}
	if (vc->Scenes().size() <= 1) {
		QMessageBox::information(this, QTStr("Vertical.RemoveScene"), QTStr("Vertical.LastScene"));
		return;
	}
	auto answer = QMessageBox::question(
		this, QTStr("Vertical.RemoveScene"),
		QTStr("Vertical.RemoveSceneConfirm").arg(QString::fromUtf8(obs_source_get_name(s))));
	if (answer == QMessageBox::Yes) {
		vc->RemoveScene(s);
	}
}

void VerticalScenesPanel::RenameScene()
{
	OBSSource s = vc->CurrentScene();
	if (!s) {
		return;
	}
	bool ok = false;
	QString name = QInputDialog::getText(this, QTStr("Vertical.RenameScene"), QTStr("Vertical.SceneName"),
					     QLineEdit::Normal, QString::fromUtf8(obs_source_get_name(s)), &ok);
	if (ok && !vc->RenameScene(s, name)) {
		QMessageBox::warning(this, QTStr("Vertical.RenameScene"), QTStr("Vertical.NameTaken"));
	}
}

/* ------------------------------------------------------------------------ */
/* Sources panel                                                            */

VerticalSourcesPanel::VerticalSourcesPanel(QWidget *parent, VerticalCanvas *vc_, VerticalPreview *preview_)
	: QWidget(parent),
	  vc(vc_),
	  preview(preview_)
{
	list = MakeList(this);
	list->installEventFilter(this);
	toolbar = MakeToolbar(this);
	addAction = ToolAction(toolbar, "icon-plus", ":/res/images/plus.svg", QTStr("Vertical.AddSourceTooltip"));
	QAction *remove = ToolAction(toolbar, "icon-trash", ":/res/images/minus.svg", QTStr("Vertical.RemoveSource"));
	toolbar->addSeparator();
	QAction *props = ToolAction(toolbar, "icon-gear", ":/settings/images/settings/general.svg",
				    QTStr("Properties"));
	toolbar->addSeparator();
	QAction *up = ToolAction(toolbar, "icon-up", ":/res/images/up.svg", QTStr("Vertical.Order.Up"));
	QAction *down = ToolAction(toolbar, "icon-down", ":/res/images/down.svg", QTStr("Vertical.Order.Down"));
	MakeFrame(this, list, toolbar);
	FinishToolbar(toolbar);

	connect(addAction, &QAction::triggered, this, &VerticalSourcesPanel::ShowAddMenu);
	connect(remove, &QAction::triggered, this, &VerticalSourcesPanel::RemoveSelected);
	connect(props, &QAction::triggered, this, [this] {
		if (OBSSceneItem item = SelectedItem()) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
		}
	});
	connect(up, &QAction::triggered, this, [this] {
		if (OBSSceneItem item = SelectedItem()) {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
		}
	});
	connect(down, &QAction::triggered, this, [this] {
		if (OBSSceneItem item = SelectedItem()) {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
		}
	});

	connect(list, &QListWidget::currentRowChanged, this, [this](int) {
		if (updating) {
			return;
		}
		QListWidgetItem *li = list->currentItem();
		OBSSceneItem target = li ? li->data(Qt::UserRole).value<OBSSceneItem>() : nullptr;
		OBSScene scene = SceneOf(vc);
		if (!scene) {
			return;
		}
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			bool want = item == static_cast<obs_sceneitem_t *>(param);
			if (obs_sceneitem_selected(item) != want) {
				obs_sceneitem_select(item, want);
			}
			return true;
		};
		obs_scene_enum_items(scene, cb, target.Get());
	});
	connect(list, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem *li) {
		OBSSceneItem item = li->data(Qt::UserRole).value<OBSSceneItem>();
		if (item) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
		}
	});
	connect(list, &QListWidget::customContextMenuRequested, this, &VerticalSourcesPanel::ShowContextMenu);

	connect(preview, &VerticalPreview::SelectionChanged, this, &VerticalSourcesPanel::QueueRefresh);
	connect(vc, &VerticalCanvas::CurrentSceneChanged, this, &VerticalSourcesPanel::ConnectScene);
	connect(vc, &VerticalCanvas::CanvasChanged, this, &VerticalSourcesPanel::ConnectScene);
	ConnectScene();
}

VerticalSourcesPanel::~VerticalSourcesPanel()
{
	sceneSignals.clear();
}

bool VerticalSourcesPanel::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == list && event->type() == QEvent::KeyPress) {
		auto *key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
			RemoveSelected();
			return true;
		}
	}
	return QWidget::eventFilter(obj, event);
}

void VerticalSourcesPanel::ConnectScene()
{
	sceneSignals.clear();
	OBSSource scene = vc->CurrentScene();
	connectedScene = OBSGetWeakRef(scene);
	if (scene) {
		signal_handler_t *sh = obs_source_get_signal_handler(scene);
		for (const char *sig : {"item_add", "item_remove", "reorder", "refresh", "item_visible", "item_locked",
					"item_select", "item_deselect"}) {
			sceneSignals.emplace_back(sh, sig, SceneSignal, this);
		}
		sceneSignals.emplace_back(sh, "item_transform", TransformSignal, this);
	}
	setEnabled(vc->GetCanvas() != nullptr);
	Refresh();
}

void VerticalSourcesPanel::SceneSignal(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalSourcesPanel *>(data);
	QMetaObject::invokeMethod(self, &VerticalSourcesPanel::QueueRefresh, Qt::QueuedConnection);
	QMetaObject::invokeMethod(self->vc, &VerticalCanvas::RequestSave, Qt::QueuedConnection);
}

void VerticalSourcesPanel::TransformSignal(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalSourcesPanel *>(data);
	QMetaObject::invokeMethod(self->vc, &VerticalCanvas::RequestSave, Qt::QueuedConnection);
}

void VerticalSourcesPanel::QueueRefresh()
{
	if (refreshQueued) {
		return;
	}
	refreshQueued = true;
	QMetaObject::invokeMethod(
		this,
		[this] {
			refreshQueued = false;
			Refresh();
		},
		Qt::QueuedConnection);
}

/* Same row as the main Sources list: icon, name, eye, lock */
QWidget *VerticalSourcesPanel::MakeRow(obs_sceneitem_t *item)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	QWidget *row = new QWidget();
	row->setAttribute(Qt::WA_TranslucentBackground);
	QHBoxLayout *layout = new QHBoxLayout(row);
	layout->setContentsMargins(4, 0, 4, 0);
	layout->setSpacing(6);

	OBSBasic *main = OBSBasic::Get();
	QLabel *icon = new QLabel(row);
	icon->setPixmap(main->GetSourceIcon(obs_source_get_id(source)).pixmap(16, 16));
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);

	QLabel *name = new QLabel(QString::fromUtf8(obs_source_get_name(source)), row);
	name->setAttribute(Qt::WA_TransparentForMouseEvents);
	if (!obs_sceneitem_visible(item)) {
		name->setEnabled(false);
	}

	QCheckBox *vis = new QCheckBox(row);
	idian::Utils::addClass(vis, "checkbox-icon");
	idian::Utils::addClass(vis, "indicator-visibility");
	vis->setChecked(obs_sceneitem_visible(item));
	vis->setToolTip(QTStr("Basic.Main.Sources.Visibility"));

	QCheckBox *lock = new QCheckBox(row);
	idian::Utils::addClass(lock, "checkbox-icon");
	idian::Utils::addClass(lock, "indicator-lock");
	lock->setChecked(obs_sceneitem_locked(item));
	lock->setToolTip(QTStr("Basic.Main.Sources.Lock"));

	layout->addWidget(icon);
	layout->addWidget(name, 1);
	layout->addWidget(vis);
	layout->addWidget(lock);

	OBSSceneItem ref = item;
	connect(vis, &QCheckBox::clicked, this, [ref](bool on) { obs_sceneitem_set_visible(ref, on); });
	connect(lock, &QCheckBox::clicked, this, [ref](bool on) { obs_sceneitem_set_locked(ref, on); });
	return row;
}

void VerticalSourcesPanel::Refresh()
{
	updating = true;
	list->clear();
	OBSScene scene = SceneOf(vc);
	if (scene) {
		std::vector<OBSSceneItem> items;
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(item);
			return true;
		};
		obs_scene_enum_items(scene, cb, &items);
		/* Top of the list = top of the picture, like OBS */
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			obs_sceneitem_t *item = *it;
			QListWidgetItem *li = new QListWidgetItem();
			li->setData(Qt::UserRole, QVariant::fromValue(OBSSceneItem(item)));
			li->setSizeHint(QSize(0, 30));
			list->addItem(li);
			list->setItemWidget(li, MakeRow(item));
			if (obs_sceneitem_selected(item)) {
				list->setCurrentItem(li);
			}
		}
	}
	updating = false;
}

OBSSceneItem VerticalSourcesPanel::SelectedItem() const
{
	QListWidgetItem *li = list->currentItem();
	return li ? li->data(Qt::UserRole).value<OBSSceneItem>() : nullptr;
}

void VerticalSourcesPanel::RemoveSelected()
{
	OBSSceneItem item = SelectedItem();
	if (!item) {
		return;
	}
	QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
	auto answer = QMessageBox::question(this, QTStr("ConfirmRemove.Title"), QTStr("ConfirmRemove.Text").arg(name));
	if (answer == QMessageBox::Yes) {
		obs_sceneitem_remove(item);
	}
}

void VerticalSourcesPanel::ShowAddMenu()
{
	OBSScene scene = SceneOf(vc);
	if (!scene) {
		return;
	}

	/* Every source with a picture; they are shared with the main canvas */
	std::vector<OBSSource> sources;
	auto cb = [](void *param, obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		}
		return true;
	};
	obs_enum_sources(cb, &sources);
	std::sort(sources.begin(), sources.end(), [](const OBSSource &a, const OBSSource &b) {
		return QString::fromUtf8(obs_source_get_name(a))
			       .compare(QString::fromUtf8(obs_source_get_name(b)), Qt::CaseInsensitive) < 0;
	});

	QMenu menu(this);
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	if (mainScene) {
		menu.addAction(QTStr("Vertical.CopyFromMain").arg(QString::fromUtf8(obs_source_get_name(mainScene))),
			       this, [this, src = OBSSource(mainScene.Get())] { vc->CopyFromMainScene(src); });
		menu.addSeparator();
	}
	if (sources.empty()) {
		menu.addAction(QTStr("Vertical.NoSources"))->setEnabled(false);
	}
	OBSBasic *main = OBSBasic::Get();
	for (const OBSSource &s : sources) {
		QAction *a = menu.addAction(main->GetSourceIcon(obs_source_get_id(s)),
					    QString::fromUtf8(obs_source_get_name(s)));
		connect(a, &QAction::triggered, this, [this, s] {
			OBSScene sc = SceneOf(vc);
			if (!sc) {
				return;
			}
			obs_sceneitem_t *item = obs_scene_add(sc, s);
			if (item) {
				VerticalCanvas::FitItem(item, VerticalCanvas::Fit::Width);
				preview->setFocus();
			}
		});
	}
	QWidget *button = toolbar->widgetForAction(addAction);
	menu.exec(button ? button->mapToGlobal(QPoint(0, button->height())) : QCursor::pos());
}

void VerticalSourcesPanel::ShowContextMenu(const QPoint &pos)
{
	QMenu menu(this);
	QListWidgetItem *li = list->itemAt(pos);
	OBSSceneItem item = li ? li->data(Qt::UserRole).value<OBSSceneItem>() : nullptr;
	if (item) {
		list->setCurrentItem(li);
		obs_source_t *source = obs_sceneitem_get_source(item);
		auto fit = [&](const QString &label, VerticalCanvas::Fit mode) {
			menu.addAction(label, this, [item, mode] { VerticalCanvas::FitItem(item, mode); });
		};
		fit(QTStr("Vertical.Fit.Width"), VerticalCanvas::Fit::Width);
		fit(QTStr("Vertical.Fit.Inside"), VerticalCanvas::Fit::Inside);
		fit(QTStr("Vertical.Fit.Fill"), VerticalCanvas::Fit::Fill);
		fit(QTStr("Vertical.Fit.Center"), VerticalCanvas::Fit::Center);
		fit(QTStr("Vertical.Fit.Reset"), VerticalCanvas::Fit::Reset);
		menu.addSeparator();
		menu.addAction(QTStr("Properties"), this,
			       [source] { obs_frontend_open_source_properties(source); });
		menu.addAction(QTStr("Filters"), this,
			       [source] { obs_frontend_open_source_filters(source); });
		menu.addSeparator();
		menu.addAction(QTStr("Vertical.RemoveSource"), this, &VerticalSourcesPanel::RemoveSelected);
		menu.addSeparator();
	}
	menu.addAction(QTStr("Vertical.AddSourceMenu"), this,
		       [this] { QTimer::singleShot(0, this, &VerticalSourcesPanel::ShowAddMenu); });
	menu.exec(list->viewport()->mapToGlobal(pos));
}
