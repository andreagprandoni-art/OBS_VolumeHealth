// Audio Health Monitor - pannello per OBS
// Legge il mix reale della traccia dello stream (LUFS con ponderazione K, gating come da ITU-R BS.1770)
// e i livelli di microfono e gioco, e mostra UN solo messaggio di stato stabile.

#include "audio-health.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-audio-controls.h>
#include <media-io/audio-io.h>

#include <QColor>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QString>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---------- Soglie (come nel pannello HTML V14) ----------
constexpr double WINDOW_MS = 5000.0;   // finestra di analisi dei picchi
constexpr double CLIP_DB = -1.0;
constexpr double HIGH_DB = -6.0;       // picchi totali troppo alti
constexpr double GATE_DB = -50.0;      // silenzio
constexpr double SPEECH_DB = -35.0;    // sopra: stai parlando
constexpr double GAME_MARGIN = 3.0;    // gioco entro 3 dB dalla voce = troppo alto
constexpr double VOICE_MARGIN = 20.0;  // voce oltre 20 dB sopra il gioco = gioco inascoltabile
constexpr double SPIKE_DB = 6.0;       // picchi di voce sopra il livello tipico
constexpr double HYST_DB = 2.0;
constexpr double HYST_LU = 1.0;
constexpr double LUFS_MIN = -18.0;
constexpr double LUFS_MAX = -14.0;
constexpr double VOICE_REF_DEFAULT = -10.0;
constexpr qint64 ENTER_MS = 3000;
constexpr qint64 CALM_MS = 2000;
constexpr qint64 MIN_HOLD_MS = 2000;
constexpr qint64 CLIP_HOLD_MS = 4000;
constexpr qint64 MISSING_MS = 8000;
constexpr qint64 LUFS_WINDOW_MS = 20000;
constexpr int LUFS_MIN_MOMENTS = 40;   // 4 s di audio (momentary da 0,4 s ogni 0,1 s)
constexpr double BAL_MIN = -5.0, BAL_MAX = 30.0;
constexpr double LBAR_MIN = -30.0, LBAR_MAX = -5.0;

QString T(const char *s)
{
	return QString::fromUtf8(s);
}

QString fmt1(double v)
{
	return QString::number(v, 'f', 1);
}

qint64 now_ms()
{
	using namespace std::chrono;
	return static_cast<qint64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

double db_clean(double v)
{
	return (std::isfinite(v) && v > -100.0) ? v : -100.0;
}

double lin_to_db(double x)
{
	return x > 1e-5 ? 20.0 * std::log10(x) : -100.0;
}

// ---------- Stati ----------
enum class St { Idle, Solo, Ok, Clip, High, Quiet, Loud, GameHigh, GameLow, VoiceHigh, Peaks, Miss };

bool isProblem(St s)
{
	switch (s) {
	case St::Idle:
	case St::Solo:
	case St::Ok:
		return false;
	default:
		return true;
	}
}

const char *stColor(St s)
{
	switch (s) {
	case St::Idle:
		return "#888888";
	case St::Solo:
	case St::Ok:
		return "#00ff66";
	case St::Clip:
		return "#ff3333";
	case St::High:
	case St::Loud:
		return "#cc66ff";
	case St::Quiet:
	case St::GameLow:
	case St::Miss:
		return "#ff9900";
	case St::GameHigh:
		return "#00ccff";
	case St::VoiceHigh:
		return "#ffffff";
	case St::Peaks:
		return "#ff6699";
	}
	return "#888888";
}

// ---------- Barra di livello / indicatore ----------
class LevelBar : public QWidget {
public:
	struct Zone {
		double from;
		double to;
		QColor color;
	};

	explicit LevelBar(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedHeight(10);
		setMinimumWidth(80);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setRange(double lo, double hi)
	{
		lo_ = lo;
		hi_ = hi;
		update();
	}
	void setZones(const std::vector<Zone> &z)
	{
		zones_ = z;
		update();
	}
	void setLevel(double v, bool valid, const QColor &c)
	{
		v_ = v;
		valid_ = valid;
		c_ = c;
		marker_ = false;
		update();
	}
	void setMarker(double v, bool valid)
	{
		v_ = v;
		valid_ = valid;
		marker_ = true;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const double h = static_cast<double>(height());
		p.fillRect(rect(), QColor(17, 17, 17));
		for (const Zone &z : zones_)
			p.fillRect(QRectF(px(z.from), 0.0, px(z.to) - px(z.from), h), z.color);
		if (!valid_)
			return;
		if (marker_)
			p.fillRect(QRectF(px(v_) - 1.5, 0.0, 3.0, h), QColor(255, 255, 255));
		else
			p.fillRect(QRectF(0.0, 0.0, px(v_), h), c_);
	}

private:
	double px(double v) const
	{
		double t = (v - lo_) / (hi_ - lo_);
		t = std::min(1.0, std::max(0.0, t));
		return t * static_cast<double>(width());
	}

	double lo_ = 0.0, hi_ = 1.0, v_ = 0.0;
	bool valid_ = false, marker_ = false;
	QColor c_;
	std::vector<Zone> zones_;
};

// ---------- Lettura del mix reale (thread audio) ----------
struct BlockOut {
	double ms;    // potenza ponderata K (somma dei canali) del blocco da 100 ms
	double peak;  // picco campione (lineare) del blocco
};

struct Biquad {
	double b0, b1, b2, a1, a2;
	double z1 = 0.0, z2 = 0.0;
	double process(double x)
	{
		const double y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

class MixTap {
public:
	~MixTap() { stop(); }

	bool start(size_t mix)
	{
		stop();
		audio_t *audio = obs_get_audio();
		if (!audio)
			return false;
		resetFilters();
		struct audio_convert_info ci = {};
		ci.samples_per_sec = 48000;
		ci.format = AUDIO_FORMAT_FLOAT_PLANAR;
		ci.speakers = SPEAKERS_STEREO;
		mix_ = mix;
		running_ = audio_output_connect(audio, mix_, &ci, &MixTap::callback, this);
		return running_;
	}

	void stop()
	{
		if (!running_)
			return;
		audio_t *audio = obs_get_audio();
		if (audio)
			audio_output_disconnect(audio, mix_, &MixTap::callback, this);
		running_ = false;
	}

	bool running() const { return running_; }

	std::vector<BlockOut> take()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<BlockOut> out;
		out.swap(out_);
		return out;
	}

private:
	static void callback(void *param, size_t, struct audio_data *data)
	{
		static_cast<MixTap *>(param)->process(data);
	}

	void resetFilters()
	{
		for (int c = 0; c < 2; c++) {
			// ITU-R BS.1770: stadio 1 (shelf) e stadio 2 (passa-alto) a 48 kHz
			shelf_[c] = Biquad{1.53512485958697, -2.69169618940638, 1.19839281085285,
					   -1.69065929318241, 0.73248077421585};
			hp_[c] = Biquad{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
		}
		sum_ = 0.0;
		peak_ = 0.0;
		count_ = 0;
	}

	void process(struct audio_data *d)
	{
		const float *ch[2] = {reinterpret_cast<const float *>(d->data[0]),
				      reinterpret_cast<const float *>(d->data[1])};
		if (!ch[0])
			return;
		if (!ch[1])
			ch[1] = ch[0];
		for (uint32_t i = 0; i < d->frames; i++) {
			for (int c = 0; c < 2; c++) {
				const double x = static_cast<double>(ch[c][i]);
				const double ax = std::fabs(x);
				if (ax > peak_)
					peak_ = ax;
				const double y = hp_[c].process(shelf_[c].process(x));
				sum_ += y * y;
			}
			if (++count_ >= 4800) {  // 100 ms a 48 kHz
				BlockOut b{sum_ / static_cast<double>(count_), peak_};
				{
					std::lock_guard<std::mutex> lock(mutex_);
					out_.push_back(b);
					if (out_.size() > 200)
						out_.erase(out_.begin());
				}
				sum_ = 0.0;
				peak_ = 0.0;
				count_ = 0;
			}
		}
	}

	bool running_ = false;
	size_t mix_ = 0;
	Biquad shelf_[2]{};
	Biquad hp_[2]{};
	double sum_ = 0.0, peak_ = 0.0;
	int count_ = 0;
	std::mutex mutex_;
	std::vector<BlockOut> out_;
};

// ---------- Lettura di una singola sorgente (mic / gioco) ----------
struct SrcMeter {
	std::mutex m;
	double hold = -100.0;
	bool clipped = false;
};

void volmeter_cb(void *param, const float[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
		 const float input_peak[MAX_AUDIO_CHANNELS])
{
	SrcMeter *s = static_cast<SrcMeter *>(param);
	double p = -100.0, ip = -100.0;
	for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		p = std::max(p, db_clean(static_cast<double>(peak[i])));
		ip = std::max(ip, db_clean(static_cast<double>(input_peak[i])));
	}
	std::lock_guard<std::mutex> lock(s->m);
	s->hold = std::max(s->hold, p);
	if (std::max(p, ip) > CLIP_DB)
		s->clipped = true;
}

class SourceTap {
public:
	~SourceTap() { detach(); }

	bool attach(const std::string &name)
	{
		detach();
		if (name.empty())
			return false;
		obs_source_t *src = obs_get_source_by_name(name.c_str());
		if (!src)
			return false;
		vm_ = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_add_callback(vm_, volmeter_cb, &meter_);
		const bool ok = obs_volmeter_attach_source(vm_, src);
		obs_source_release(src);
		if (!ok)
			detach();
		return ok;
	}

	void detach()
	{
		if (!vm_)
			return;
		obs_volmeter_remove_callback(vm_, volmeter_cb, &meter_);
		obs_volmeter_destroy(vm_);
		vm_ = nullptr;
	}

	bool attached() const { return vm_ != nullptr; }

	void take(double &hold, bool &clipped)
	{
		std::lock_guard<std::mutex> lock(meter_.m);
		hold = meter_.hold;
		clipped = meter_.clipped;
		meter_.hold = -100.0;
		meter_.clipped = false;
	}

private:
	obs_volmeter_t *vm_ = nullptr;
	SrcMeter meter_;
};

bool enum_audio_cb(void *param, obs_source_t *src)
{
	if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
		const char *n = obs_source_get_name(src);
		if (n)
			static_cast<std::vector<std::string> *>(param)->push_back(n);
	}
	return true;
}

void fillCombo(QComboBox *box, const std::vector<std::string> &names, const QString &want)
{
	QSignalBlocker block(box);
	box->clear();
	bool found = false;
	for (const std::string &n : names) {
		const QString q = QString::fromStdString(n);
		box->addItem(q);
		if (q == want)
			found = true;
	}
	if (!found && !want.isEmpty())
		box->addItem(want);
	box->setCurrentText(want);
}

// ---------- Pannello ----------
class HealthDock : public QWidget {
public:
	HealthDock() : QWidget(nullptr)
	{
		QSettings s(QStringLiteral("obs-audio-health"), QStringLiteral("settings"));
		micName_ = s.value(QStringLiteral("mic"), QStringLiteral("MIC - Nvidia Broadcast")).toString().toStdString();
		gameName_ = s.value(QStringLiteral("game"), QStringLiteral("NDI Source")).toString().toStdString();
		track_ = s.value(QStringLiteral("track"), 0).toInt();
		if (s.contains(QStringLiteral("voiceRef"))) {
			voiceRef_ = s.value(QStringLiteral("voiceRef")).toDouble();
			voiceN_ = 100;
		}
		if (track_ < 0 || track_ > 5)
			track_ = 0;

		startedAt_ = now_ms();
		since_ = startedAt_;

		buildUi();
		refreshSources();
		trackCombo_->setCurrentIndex(track_);
		applySelection();

		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, [this]() { tick(); });
		timer_->start(100);
	}

	~HealthDock() override { shutdown(); }

	void shutdown()
	{
		if (down_)
			return;
		down_ = true;
		if (timer_)
			timer_->stop();
		mix_.stop();
		micTap_.detach();
		gameTap_.detach();
		QSettings s(QStringLiteral("obs-audio-health"), QStringLiteral("settings"));
		s.setValue(QStringLiteral("voiceRef"), voiceRef_);
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		refreshSources();
	}

private:
	// ----- interfaccia -----
	void buildUi()
	{
		QVBoxLayout *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);

		status_ = new QLabel(T("In attesa di audio..."));
		status_->setWordWrap(true);
		status_->setMinimumHeight(52);
		setStatus(T("In attesa di audio..."), St::Idle);
		root->addWidget(status_);

		micCombo_ = new QComboBox();
		gameCombo_ = new QComboBox();
		trackCombo_ = new QComboBox();
		for (int i = 1; i <= 6; i++)
			trackCombo_->addItem(T("Traccia ") + QString::number(i));
		QPushButton *refresh = new QPushButton(T("Aggiorna elenco sorgenti"));

		QGridLayout *form = new QGridLayout();
		form->setColumnStretch(1, 1);
		form->addWidget(new QLabel(T("Mic")), 0, 0);
		form->addWidget(micCombo_, 0, 1);
		form->addWidget(new QLabel(T("Gioco")), 1, 0);
		form->addWidget(gameCombo_, 1, 1);
		form->addWidget(new QLabel(T("Stream su")), 2, 0);
		form->addWidget(trackCombo_, 2, 1);
		form->addWidget(refresh, 3, 1);
		root->addLayout(form);

		QGridLayout *mg = new QGridLayout();
		mg->setColumnStretch(1, 1);
		auto addMeter = [&](int row, const char *label, LevelBar *&bar, QLabel *&val) {
			bar = new LevelBar();
			val = new QLabel(T("-"));
			val->setMinimumWidth(84);
			val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			mg->addWidget(new QLabel(T(label)), row, 0);
			mg->addWidget(bar, row, 1);
			mg->addWidget(val, row, 2);
		};
		addMeter(0, "Mic", micBar_, micVal_);
		addMeter(1, "Gioco", gameBar_, gameVal_);
		addMeter(2, "Picchi", mixBar_, mixVal_);
		addMeter(3, "Mix", balBar_, balVal_);
		addMeter(4, "LUFS", lufsBar_, lufsVal_);
		root->addLayout(mg);

		micBar_->setRange(-60.0, 0.0);
		gameBar_->setRange(-60.0, 0.0);
		mixBar_->setRange(-60.0, 0.0);
		balBar_->setRange(BAL_MIN, BAL_MAX);
		balBar_->setZones({{BAL_MIN, GAME_MARGIN, QColor(0x7a, 0x2b, 0x2b)},
				   {GAME_MARGIN, VOICE_MARGIN, QColor(0x1f, 0x6b, 0x3a)},
				   {VOICE_MARGIN, BAL_MAX, QColor(0x7a, 0x5a, 0x1f)}});
		lufsBar_->setRange(LBAR_MIN, LBAR_MAX);
		lufsBar_->setZones({{LBAR_MIN, LUFS_MIN, QColor(0x7a, 0x5a, 0x1f)},
				    {LUFS_MIN, LUFS_MAX, QColor(0x1f, 0x6b, 0x3a)},
				    {LUFS_MAX, LBAR_MAX, QColor(0x7a, 0x2b, 0x2b)}});

		balNote_ = new QLabel(T("Equilibrio voce / gioco"));
		lufsNote_ = new QLabel();
		for (QLabel *l : {balNote_, lufsNote_}) {
			l->setWordWrap(true);
			l->setStyleSheet(QStringLiteral("color:#888;font-size:10px;"));
			root->addWidget(l);
		}
		baseLufsNote_ = T("Loudness del mix: ok tra ") + QString::number(LUFS_MIN, 'f', 0) + T(" e ") +
				QString::number(LUFS_MAX, 'f', 0) + T(" LUFS");
		lufsNote_->setText(baseLufsNote_);

		QLabel *hist = new QLabel(T("STORICO PROBLEMI"));
		hist->setStyleSheet(QStringLiteral("color:#888;font-size:10px;font-weight:bold;"));
		root->addWidget(hist);
		log_ = new QPlainTextEdit();
		log_->setReadOnly(true);
		log_->setMaximumBlockCount(25);
		log_->setMinimumHeight(80);
		root->addWidget(log_, 1);

		connect(micCombo_, &QComboBox::currentTextChanged, this, [this](const QString &) { applySelection(); });
		connect(gameCombo_, &QComboBox::currentTextChanged, this, [this](const QString &) { applySelection(); });
		connect(trackCombo_, &QComboBox::currentTextChanged, this, [this](const QString &) { applySelection(); });
		connect(refresh, &QPushButton::clicked, this, [this]() {
			refreshSources();
			applySelection();
		});
	}

	void setStatus(const QString &text, St st)
	{
		const QString col = QString::fromLatin1(stColor(st));
		status_->setText(text);
		status_->setStyleSheet(QStringLiteral("QLabel{background:#141414;color:%1;border:1px solid #333;"
						      "border-left:4px solid %1;padding:8px;font-weight:bold;font-size:13px;}")
					       .arg(col));
	}

	void addLog(const QString &text)
	{
		log_->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + text);
	}

	void refreshSources()
	{
		std::vector<std::string> names;
		obs_enum_sources(enum_audio_cb, &names);
		std::sort(names.begin(), names.end());
		const QString wantMic = micCombo_->count() ? micCombo_->currentText() : QString::fromStdString(micName_);
		const QString wantGame = gameCombo_->count() ? gameCombo_->currentText() : QString::fromStdString(gameName_);
		fillCombo(micCombo_, names, wantMic);
		fillCombo(gameCombo_, names, wantGame);
	}

	void applySelection()
	{
		micName_ = micCombo_->currentText().toStdString();
		gameName_ = gameCombo_->currentText().toStdString();
		track_ = std::max(0, trackCombo_->currentIndex());
		micTap_.attach(micName_);
		gameTap_.attach(gameName_);
		mix_.start(static_cast<size_t>(track_));

		QSettings s(QStringLiteral("obs-audio-health"), QStringLiteral("settings"));
		s.setValue(QStringLiteral("mic"), micCombo_->currentText());
		s.setValue(QStringLiteral("game"), gameCombo_->currentText());
		s.setValue(QStringLiteral("track"), track_);
	}

	// ----- loudness (gating come da ITU-R BS.1770) -----
	bool gatedLufs(double &out) const
	{
		const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
		double sum1 = 0.0;
		int n1 = 0;
		for (const Moment &m : moments_) {
			if (m.ms > absGate) {
				sum1 += m.ms;
				n1++;
			}
		}
		if (n1 < LUFS_MIN_MOMENTS)
			return false;
		const double mean1 = sum1 / static_cast<double>(n1);
		double sum2 = 0.0;
		int n2 = 0;
		for (const Moment &m : moments_) {
			if (m.ms > absGate && m.ms > mean1 * 0.1) {
				sum2 += m.ms;
				n2++;
			}
		}
		if (n2 == 0)
			return false;
		out = 10.0 * std::log10(sum2 / static_cast<double>(n2)) - 0.691;
		return true;
	}

	QColor levelColor(double db) const
	{
		if (db > CLIP_DB)
			return QColor(255, 51, 51);
		if (db > HIGH_DB)
			return QColor(255, 204, 0);
		return QColor(0, 255, 102);
	}

	QString dbText(double db) const { return db <= -100.0 ? T("-inf") : fmt1(db) + T(" dB"); }

	// ----- ciclo principale (10 volte al secondo) -----
	void tick()
	{
		if (down_)
			return;
		const qint64 now = now_ms();

		if (now - lastAttach_ > 3000) {
			lastAttach_ = now;
			if (!micTap_.attached() && !micName_.empty())
				micTap_.attach(micName_);
			if (!gameTap_.attached() && !gameName_.empty())
				gameTap_.attach(gameName_);
		}

		// Mix reale della traccia
		double mixPeakLin = 0.0;
		for (const BlockOut &b : mix_.take()) {
			mixPeakLin = std::max(mixPeakLin, b.peak);
			recent_.push_back(b.ms);
			if (recent_.size() > 4)
				recent_.pop_front();
			if (recent_.size() == 4) {
				double s = 0.0;
				for (double v : recent_)
					s += v;
				moments_.push_back(Moment{now, s / 4.0});
			}
		}
		while (!moments_.empty() && moments_.front().t < now - LUFS_WINDOW_MS)
			moments_.pop_front();
		const double totDb = lin_to_db(mixPeakLin);

		// Microfono e gioco
		double micDb = -100.0, gameDb = -100.0;
		bool micClip = false, gameClip = false;
		micTap_.take(micDb, micClip);
		gameTap_.take(gameDb, gameClip);

		double lufs = 0.0;
		const bool haveLufs = gatedLufs(lufs);

		// Finestra scorrevole dei picchi
		samples_.push_back(Sample{now, micDb, gameDb, totDb});
		while (!samples_.empty() && samples_.front().t < now - static_cast<qint64>(WINDOW_MS))
			samples_.pop_front();
		double m = -100.0, g = -100.0, master = -100.0;
		for (const Sample &s : samples_) {
			m = std::max(m, s.mic);
			g = std::max(g, s.game);
			master = std::max(master, s.tot);
		}
		const bool speaking = m > SPEECH_DB;

		// Livello tipico della voce (si impara mentre parli; i picchi pesano pochissimo)
		if (speaking) {
			voiceN_ = std::min(voiceN_ + 1, 250);
			const bool spike = voiceN_ > 10 && m > voiceRef_ + SPIKE_DB;
			const double div = spike ? static_cast<double>(voiceN_) * 8.0 : static_cast<double>(voiceN_);
			voiceRef_ = std::max(-24.0, std::min(-6.0, voiceRef_ + (m - voiceRef_) / div));
			if (now - lastSave_ > 30000) {
				lastSave_ = now;
				QSettings st(QStringLiteral("obs-audio-health"), QStringLiteral("settings"));
				st.setValue(QStringLiteral("voiceRef"), voiceRef_);
			}
		}
		if (micClip || gameClip) {
			lastClip_ = now;
			clipWho_ = micClip ? T("MIC") : T("GIOCO");
		}
		if (totDb > CLIP_DB) {
			lastClip_ = now;
			clipWho_ = T("TOTALE");
		}

		updateMeters(micDb, gameDb, totDb, m, g, master, speaking, haveLufs, lufs);
		runState(now, m, g, master, speaking, haveLufs, lufs);
	}

	void updateMeters(double micDb, double gameDb, double totDb, double m, double g, double master, bool speaking,
			  bool haveLufs, double lufs)
	{
		micBar_->setLevel(micDb, micDb > -100.0, levelColor(micDb));
		gameBar_->setLevel(gameDb, gameDb > -100.0, levelColor(gameDb));
		mixBar_->setLevel(totDb, totDb > -100.0, levelColor(totDb));
		micVal_->setText(dbText(micDb));
		gameVal_->setText(dbText(gameDb));
		mixVal_->setText(dbText(totDb));

		if (g < GATE_DB) {
			balBar_->setMarker(0.0, false);
			balVal_->setText(T("-"));
			balNote_->setText(T("Gioco non rilevato o silenzioso: equilibrio non valutabile."));
		} else {
			const double d = (speaking ? m : voiceRef_) - g;
			const QString who = speaking ? T("voce") : T("voce tipica");
			const QString range = T("ok tra ") + QString::number(GAME_MARGIN, 'f', 0) + T(" e ") +
					      QString::number(VOICE_MARGIN, 'f', 0) + T(" dB sopra il gioco");
			balBar_->setMarker(d, true);
			balVal_->setText(QString(d >= 0.0 ? "+" : "") + fmt1(d) + T(" dB"));
			if (d < GAME_MARGIN)
				balNote_->setText(T("Il gioco sovrasta la ") + who + T(" (") + range + T(")"));
			else if (d > VOICE_MARGIN)
				balNote_->setText(T("Il gioco è quasi inudibile sotto la ") + who + T(" (") + range + T(")"));
			else
				balNote_->setText(T("Equilibrio OK: ") + who + T(" ") + fmt1(d) + T(" dB sopra il gioco (") + range + T(")"));
		}

		if (haveLufs) {
			lufsBar_->setMarker(lufs, true);
			lufsVal_->setText(fmt1(lufs) + T(" LUFS"));
			if (master > GATE_DB) {
				const double plr = master - lufs;
				const double plrMax = HIGH_DB - LUFS_MIN;
				lufsNote_->setText(baseLufsNote_ + T(" · dinamica picco-LUFS ") + fmt1(plr) + T(" dB (max ") +
						   QString::number(plrMax, 'f', 0) + T(")"));
			} else {
				lufsNote_->setText(baseLufsNote_);
			}
		} else {
			lufsBar_->setMarker(0.0, false);
			lufsVal_->setText(T("-"));
			lufsNote_->setText(baseLufsNote_);
		}
	}

	void runState(qint64 now, double m, double g, double master, bool speaking, bool haveLufs, double lufs)
	{
		const St cur = cur_;
		auto h = [&](St k) { return cur == k ? HYST_DB : 0.0; };
		auto hl = [&](St k) { return cur == k ? HYST_LU : 0.0; };
		const double hi = voiceRef_ - GAME_MARGIN;
		const double lo = voiceRef_ - VOICE_MARGIN;
		const QString lt = haveLufs ? T("live ") + fmt1(lufs) + T(" LUFS") : T("misuro i LUFS...");

		QString lost;
		if (now - startedAt_ > MISSING_MS) {
			if (!micTap_.attached())
				lost = T("MIC");
			else if (!gameTap_.attached())
				lost = T("GIOCO");
		}

		St cand = St::Idle;
		QString msg;
		auto set = [&](St s, const QString &t) {
			cand = s;
			msg = t;
		};

		if (!mix_.running())
			set(St::Miss, T("Audio di OBS non disponibile per la traccia selezionata."));
		else if (!lost.isEmpty())
			set(St::Miss, T("Sorgente ") + lost + T(" non trovata: sceglila dal menu e premi Aggiorna elenco."));
		else if (now - lastClip_ < CLIP_HOLD_MS)
			set(St::Clip, T("CLIPPING ") + clipWho_ + T("! Riduci il guadagno."));
		else if (master < GATE_DB)
			set(St::Idle, T("In attesa di audio (silenzio)."));
		else if (master > HIGH_DB - h(St::High))
			set(St::High, T("PICCHI TOTALI TROPPO ALTI (") + fmt1(master) + T(" dB). Rischio saturazione!"));
		else if (!speaking) {
			if (g < GATE_DB)
				set(St::Idle, T("Nessuna voce né gioco in riproduzione."));
			else if (g > hi - h(St::GameHigh))
				set(St::GameHigh, T("GIOCO TROPPO ALTO per parlarci sopra (") + fmt1(g) + T(" dB, massimo ") +
							  fmt1(hi) + T(" dB)."));
			else if (g < lo + h(St::GameLow))
				set(St::GameLow, T("GIOCO TROPPO BASSO (") + fmt1(g) + T(" dB, minimo ") + fmt1(lo) +
							 T(" dB): sotto la voce non si sentirebbe."));
			else
				set(St::Solo, T("NON STAI PARLANDO: gioco OK (") + fmt1(g) + T(" dB), resta bilanciato quando parli."));
		} else if (haveLufs && lufs < LUFS_MIN + hl(St::Quiet)) {
			if (master - lufs > HIGH_DB - LUFS_MIN)
				set(St::Quiet, T("LIVE BASSA (") + fmt1(lufs) + T(" LUFS) PER TROPPA DINAMICA: picchi a ") + fmt1(master) +
						       T(" dB, ") + fmt1(master - lufs) +
						       T(" dB sopra la media. Serve più compressione, non più volume."));
			else
				set(St::Quiet, T("LIVE TROPPO BASSA: ") + fmt1(lufs) + T(" LUFS (minimo ") +
						       QString::number(LUFS_MIN, 'f', 0) + T("). Alza il volume."));
		} else if (haveLufs && lufs > LUFS_MAX - hl(St::Loud)) {
			set(St::Loud, T("LIVE TROPPO ALTA: ") + fmt1(lufs) + T(" LUFS (massimo ") + QString::number(LUFS_MAX, 'f', 0) +
					      T("). Troppo forte per gli spettatori."));
		} else if (g > m - GAME_MARGIN - h(St::GameHigh)) {
			set(St::GameHigh, T("GIOCO TROPPO ALTO: sovrasta la voce (gioco ") + fmt1(g) + T(" dB, voce ") + fmt1(m) + T(" dB)."));
		} else if (g > GATE_DB && m > g + VOICE_MARGIN - h(St::VoiceHigh)) {
			set(St::VoiceHigh, T("GIOCO QUASI INASCOLTABILE: la voce lo sovrasta di ") + fmt1(m - g) + T(" dB (limite ") +
						   QString::number(VOICE_MARGIN, 'f', 0) + T(" dB)."));
		} else if (m > voiceRef_ + SPIKE_DB - h(St::Peaks)) {
			set(St::Peaks, T("PICCHI DI VOCE: ") + fmt1(m - voiceRef_) + T(" dB sopra il tuo livello tipico (") +
					       fmt1(voiceRef_) + T(" dB)."));
		} else if (g > GATE_DB) {
			set(St::Ok, T("TUTTO OK: ") + lt + T(", voce ") + fmt1(m - g) + T(" dB sopra il gioco."));
		} else {
			set(St::Ok, T("TUTTO OK: ") + lt + T(" (gioco silenzioso)."));
		}

		// Lo stato cambia solo se il candidato resta stabile (anti-sfarfallio)
		if (cand != cand_) {
			cand_ = cand;
			candSince_ = now;
		}
		const qint64 delay = cand == St::Clip ? 0 : (isProblem(cand) ? ENTER_MS : CALM_MS);
		if (cand != cur_ && now - candSince_ >= delay && (cand == St::Clip || now - since_ >= MIN_HOLD_MS)) {
			const bool was = isProblem(cur_);
			cur_ = cand;
			since_ = now;
			lastUi_ = 0;
			if (isProblem(cand))
				addLog(msg);
			else if (was)
				addLog(T("Rientrato nella norma."));
		}
		if (cand == cur_)
			msg_ = msg;
		if (now - lastUi_ > 500) {
			lastUi_ = now;
			setStatus(msg_, cur_);
		}
	}

	struct Moment {
		qint64 t;
		double ms;
	};
	struct Sample {
		qint64 t;
		double mic, game, tot;
	};

	MixTap mix_;
	SourceTap micTap_, gameTap_;
	std::string micName_, gameName_;
	int track_ = 0;

	std::deque<double> recent_;
	std::deque<Moment> moments_;
	std::deque<Sample> samples_;

	double voiceRef_ = VOICE_REF_DEFAULT;
	int voiceN_ = 0;
	qint64 lastSave_ = 0, lastAttach_ = 0, lastClip_ = -100000, lastUi_ = 0;
	qint64 startedAt_ = 0, since_ = 0, candSince_ = 0;
	QString clipWho_;

	St cur_ = St::Idle;
	St cand_ = St::Idle;
	QString msg_ = QStringLiteral("In attesa di audio...");

	bool down_ = false;
	QTimer *timer_ = nullptr;
	QLabel *status_ = nullptr, *micVal_ = nullptr, *gameVal_ = nullptr, *mixVal_ = nullptr, *balVal_ = nullptr,
	       *lufsVal_ = nullptr, *balNote_ = nullptr, *lufsNote_ = nullptr;
	QString baseLufsNote_;
	LevelBar *micBar_ = nullptr, *gameBar_ = nullptr, *mixBar_ = nullptr, *balBar_ = nullptr, *lufsBar_ = nullptr;
	QComboBox *micCombo_ = nullptr, *gameCombo_ = nullptr, *trackCombo_ = nullptr;
	QPlainTextEdit *log_ = nullptr;
};

QPointer<HealthDock> g_dock;

} // namespace

void audio_health_create_dock()
{
	HealthDock *dock = new HealthDock();
	g_dock = dock;
	if (!obs_frontend_add_dock_by_id("audio-health-monitor", "Audio Health Monitor", static_cast<QWidget *>(dock))) {
		blog(LOG_WARNING, "[audio-health] impossibile registrare il pannello");
		dock->shutdown();
		delete dock;
		g_dock.clear();
	}
}

void audio_health_shutdown()
{
	if (g_dock)
		g_dock->shutdown();
}
