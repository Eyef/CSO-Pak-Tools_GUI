// NAR archive actions of the main window: pack a folder into a .nar, patch a
// .nar with replacement files, unpack a .nar to a folder. They mirror the CLI
// commands nar-pack / nar-patch / nar-unpack and call the same core functions
// (core/NarTools.h). Kept out of MainWindow.cpp to leave that file alone.
#include "MainWindow.h"

#include <filesystem>
#include <stdexcept>
#include <string>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>
#include <QVBoxLayout>

#include "NarArchive.h"
#include "NarTools.h"

namespace
{
	std::filesystem::path ToPath(const QString &s)
	{
		return std::filesystem::path(s.toStdU16String());
	}

	struct Cancelled : std::runtime_error
	{
		Cancelled() : std::runtime_error("cancelled") {}
	};

	// Progress dialog driven from the (single) worker == UI thread.
	class Progress
	{
	public:
		Progress(QWidget *parent, const QString &title)
			: dlg_(title, QObject::tr("Cancel"), 0, 100, parent)
		{
			dlg_.setWindowModality(Qt::WindowModal);
			dlg_.setMinimumDuration(0);
			dlg_.setValue(0);
		}
		cso_pak::NarProgressFn Callback()
		{
			return [this](size_t done, size_t total, const std::string &path) {
				dlg_.setMaximum(total > 0 ? static_cast<int>(total) : 1);
				dlg_.setValue(static_cast<int>(done));
				dlg_.setLabelText(QString::fromLocal8Bit(path.c_str()));
				QApplication::processEvents();
				if (dlg_.wasCanceled())
					throw Cancelled();
			};
		}
		void Finish() { dlg_.setValue(dlg_.maximum()); }
	private:
		QProgressDialog dlg_;
	};

	QString JoinLines(const std::vector<std::string> &v, size_t limit = 15)
	{
		QStringList lines;
		for (size_t i = 0; i < v.size() && i < limit; ++i)
			lines << QString::fromLocal8Bit(v[i].c_str());
		if (v.size() > limit)
			lines << QObject::tr("... and %1 more").arg(v.size() - limit);
		return lines.join(QLatin1Char('\n'));
	}

	// Store type combo: index -> NarStoreType.
	QComboBox *MakeStoreCombo(QWidget *parent, int current)
	{
		auto *c = new QComboBox(parent);
		c->addItem(QObject::tr("Encoded (XOR) - default"), 1);
		c->addItem(QObject::tr("Encoded + compressed (LZ)"), 2);
		c->addItem(QObject::tr("Raw (no encoding)"), 0);
		c->setCurrentIndex(c->findData(current));
		return c;
	}

	QComboBox *MakeIceCombo(QWidget *parent)
	{
		auto *c = new QComboBox(parent);
		c->addItem(QObject::tr("Leave models as they are"), 0);
		c->addItem(QObject::tr("Encrypt with ICE, version 21"), 21);
		c->addItem(QObject::tr("Encrypt with ICE, version 20"), 20);
		return c;
	}

	cso_pak::NarStoreType StoreOf(const QComboBox *c)
	{
		return static_cast<cso_pak::NarStoreType>(c->currentData().toInt());
	}
}

void MainWindow::OnPackNar()
{
	const QString inputDir = QFileDialog::getExistingDirectory(this,
		tr("Choose directory to pack into a NAR"));
	if (inputDir.isEmpty())
		return;

	QDialog dlg(this);
	dlg.setWindowTitle(tr("Pack Directory into New NAR"));
	auto *form = new QFormLayout;
	auto *typeBox = MakeStoreCombo(&dlg, 1);
	auto *iceBox = MakeIceCombo(&dlg);
	auto *manifestCheck = new QCheckBox(tr("Use .nar-info.json if present (restores store types, ICE and order)"), &dlg);
	manifestCheck->setChecked(true);
	auto *rootEdit = new QLineEdit(&dlg);
	rootEdit->setPlaceholderText(tr("empty = use the folder name"));
	auto *noRoot = new QCheckBox(tr("No root folder (paths start directly in the chosen directory)"), &dlg);
	form->addRow(tr("Store type for new files:"), typeBox);
	form->addRow(tr("Plain .mdl files:"), iceBox);
	form->addRow(QString(), manifestCheck);
	form->addRow(tr("Archive root folder:"), rootEdit);
	form->addRow(QString(), noRoot);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	auto *layout = new QVBoxLayout(&dlg);
	layout->addLayout(form);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString outputNar = QFileDialog::getSaveFileName(this, tr("Save new NAR Archive"),
		QString(), tr("NAR Archives (*.nar)"));
	if (outputNar.isEmpty())
		return;

	cso_pak::NarPackOptions options;
	options.defaultType = StoreOf(typeBox);
	options.encryptModels = iceBox->currentData().toInt();
	options.useManifest = manifestCheck->isChecked();
	if (noRoot->isChecked())
	{
		options.hasRoot = true;
	}
	else if (!rootEdit->text().trimmed().isEmpty())
	{
		options.hasRoot = true;
		options.root = rootEdit->text().trimmed().toStdString();
	}

	try
	{
		Progress progress(this, tr("Packing NAR..."));
		const auto stats = cso_pak::PackDirectoryToNar(ToPath(inputDir), ToPath(outputNar),
			options, progress.Callback());
		progress.Finish();

		QString summary = tr("Packed %1 file(s) into %2.").arg(stats.files).arg(QFileInfo(outputNar).fileName());
		if (stats.usedManifest)
			summary += QStringLiteral("\n") + tr("Used .nar-info.json.");
		if (stats.modelsEncrypted > 0)
			summary += QStringLiteral("\n") + tr("ICE-encrypted models: %1.").arg(stats.modelsEncrypted);
		if (!stats.warnings.empty())
			summary += QStringLiteral("\n\n") + tr("Warnings:") + QStringLiteral("\n") + JoinLines(stats.warnings);

		if (QMessageBox::question(this, tr("Pack complete"),
				summary + QStringLiteral("\n\n") + tr("Open the new archive now?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
		{
			LoadPak(outputNar);
		}
	}
	catch (const Cancelled &)
	{
		statusBar()->showMessage(tr("NAR packing cancelled."), 5000);
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Pack failed"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnPatchNar()
{
	const QString sourceNar = QFileDialog::getOpenFileName(this, tr("Choose source NAR Archive"),
		QString(), tr("NAR Archives (*.nar);;All Files (*)"));
	if (sourceNar.isEmpty())
		return;

	const QString replacementDir = QFileDialog::getExistingDirectory(this,
		tr("Choose replacement files directory"));
	if (replacementDir.isEmpty())
		return;

	QDialog dlg(this);
	dlg.setWindowTitle(tr("Patch NAR with Replacements"));
	auto *form = new QFormLayout;
	auto *addNew = new QCheckBox(tr("Also add files that are not in the archive"), &dlg);
	auto *typeBox = MakeStoreCombo(&dlg, 1);
	auto *iceBox = MakeIceCombo(&dlg);
	auto *rootEdit = new QLineEdit(&dlg);
	rootEdit->setPlaceholderText(tr("e.g. cstrike"));
	form->addRow(QString(), addNew);
	form->addRow(tr("Store type for new files:"), typeBox);
	form->addRow(tr("New plain .mdl files:"), iceBox);
	form->addRow(tr("Root folder for new files:"), rootEdit);
	auto *note = new QLabel(tr("Replaced models that were ICE-encrypted stay encrypted automatically."), &dlg);
	note->setWordWrap(true);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	auto *layout = new QVBoxLayout(&dlg);
	layout->addLayout(form);
	layout->addWidget(note);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString outputNar = QFileDialog::getSaveFileName(this, tr("Save patched NAR Archive"),
		QString(), tr("NAR Archives (*.nar)"));
	if (outputNar.isEmpty())
		return;
	if (QFileInfo(outputNar).absoluteFilePath().compare(
			QFileInfo(sourceNar).absoluteFilePath(), Qt::CaseInsensitive) == 0)
	{
		QMessageBox::warning(this, tr("Patch failed"),
			tr("The output must be a different file than the source archive."));
		return;
	}

	cso_pak::NarPatchOptions options;
	options.addNew = addNew->isChecked();
	options.newFileType = StoreOf(typeBox);
	options.encryptNewModels = iceBox->currentData().toInt();
	options.root = rootEdit->text().trimmed().toStdString();

	try
	{
		const auto source = cso_pak::NarArchive::Load(ToPath(sourceNar));
		Progress progress(this, tr("Patching NAR..."));
		const auto stats = cso_pak::PatchNar(source, ToPath(replacementDir), ToPath(outputNar),
			options, progress.Callback());
		progress.Finish();

		QString summary = tr("Patched: %1 replaced, %2 added, %3 preserved unchanged.\nWritten to %4.")
			.arg(stats.replaced).arg(stats.added).arg(stats.preserved)
			.arg(QFileInfo(outputNar).fileName());
		if (!stats.skipped.empty())
			summary += QStringLiteral("\n\n") + tr("Skipped (no match / ambiguous):") +
				QStringLiteral("\n") + JoinLines(stats.skipped);

		if (QMessageBox::question(this, tr("Patch complete"),
				summary + QStringLiteral("\n\n") + tr("Open the new archive now?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
		{
			LoadPak(outputNar);
		}
	}
	catch (const Cancelled &)
	{
		statusBar()->showMessage(tr("NAR patching cancelled."), 5000);
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Patch failed"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnUnpackNar()
{
	const QString sourceNar = QFileDialog::getOpenFileName(this, tr("Choose NAR Archive to unpack"),
		QString(), tr("NAR Archives (*.nar);;All Files (*)"));
	if (sourceNar.isEmpty())
		return;

	QDialog dlg(this);
	dlg.setWindowTitle(tr("Unpack NAR to Directory"));
	auto *decrypt = new QCheckBox(tr("Decrypt ICE-encrypted models (.mdl v20/v21)"), &dlg);
	decrypt->setChecked(true);
	auto *manifest = new QCheckBox(tr("Write .nar-info.json (lets you repack with the same settings)"), &dlg);
	manifest->setChecked(true);
	auto *verify = new QCheckBox(tr("Verify CRC32 of every entry"), &dlg);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	auto *layout = new QVBoxLayout(&dlg);
	layout->addWidget(decrypt);
	layout->addWidget(manifest);
	layout->addWidget(verify);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString outputDir = QFileDialog::getExistingDirectory(this,
		tr("Choose output directory"));
	if (outputDir.isEmpty())
		return;

	cso_pak::NarExtractOptions options;
	options.decryptModels = decrypt->isChecked();
	options.writeManifest = manifest->isChecked();
	options.verifyCrc = verify->isChecked();

	try
	{
		const auto source = cso_pak::NarArchive::Load(ToPath(sourceNar));
		Progress progress(this, tr("Unpacking NAR..."));
		const auto stats = cso_pak::ExtractNarToDirectory(source, ToPath(outputDir), options,
			progress.Callback());
		progress.Finish();

		QString summary = tr("Extracted %1 of %2 file(s) to %3.").arg(stats.written).arg(stats.total).arg(outputDir);
		if (stats.modelsDecrypted > 0)
			summary += QStringLiteral("\n") + tr("Decrypted models: %1.").arg(stats.modelsDecrypted);
		if (stats.failed > 0)
		{
			summary += QStringLiteral("\n\n") + tr("%1 entr(ies) failed:").arg(stats.failed) +
				QStringLiteral("\n") + JoinLines(stats.errors);
			QMessageBox::warning(this, tr("Unpack finished with errors"), summary);
		}
		else
		{
			QMessageBox::information(this, tr("Unpack complete"), summary);
		}
	}
	catch (const Cancelled &)
	{
		statusBar()->showMessage(tr("NAR unpacking cancelled."), 5000);
	}
	catch (const std::exception &ex)
	{
		QMessageBox::critical(this, tr("Unpack failed"), QString::fromUtf8(ex.what()));
	}
}

void MainWindow::OnVerifyNarIntegrity()
{
	if (narArchives_.empty())
		return;

	qint64 total = 0;
	for (const auto &nar : narArchives_)
		total += static_cast<qint64>(nar->Entries().size());

	QProgressDialog progress(tr("Verifying NAR archives..."), tr("Cancel"), 0,
		static_cast<int>(total), this);
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(400);

	qint64 done = 0;
	int verified = 0, mismatched = 0, skipped = 0;
	bool cancelled = false;
	QStringList details;

	for (const auto &nar : narArchives_)
	{
		const QString archiveName = QFileInfo(QString::fromStdU16String(
			nar->SourcePath().u16string())).fileName();
		for (const auto &entry : nar->Entries())
		{
			progress.setValue(static_cast<int>(done));
			progress.setLabelText(tr("Verifying %1 (%2 of %3)...")
				.arg(archiveName).arg(done).arg(total));
			QApplication::processEvents();
			if (progress.wasCanceled())
			{
				cancelled = true;
				break;
			}
			++done;

			if (entry.checksum == 0 || entry.storedSize == 0)
			{
				++skipped;  // checksum left unset for this entry
				continue;
			}
			try
			{
				if (nar->VerifyEntry(entry))
				{
					++verified;
					continue;
				}
				++mismatched;
				if (details.size() < 100)
					details << QString::fromLocal8Bit(entry.path.c_str());
			}
			catch (const std::exception &ex)
			{
				++mismatched;
				if (details.size() < 100)
					details << QStringLiteral("%1: %2").arg(
						QString::fromLocal8Bit(entry.path.c_str()), QString::fromUtf8(ex.what()));
			}
		}
		if (cancelled)
			break;
	}
	progress.setValue(static_cast<int>(total));

	QString msg = tr("Verification finished.\n\nTotal entries: %1\nVerified OK: %2\n"
		"Mismatched: %3\nSkipped (no checksum): %4")
		.arg(verified + mismatched + skipped).arg(verified).arg(mismatched).arg(skipped);
	if (cancelled)
		msg += tr("\n(Cancelled partway through.)");
	if (!details.isEmpty())
		msg += tr("\n\nMismatching entries:\n%1").arg(details.join(QLatin1Char('\n')));

	if (mismatched == 0)
		QMessageBox::information(this, tr("Verify NAR archives"), msg);
	else
		QMessageBox::warning(this, tr("Verify NAR archives"), msg);
}
