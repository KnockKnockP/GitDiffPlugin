#include "GitAssetDiffModule.h"

#include "AssetDefinition.h"
#include "AssetRegistry/AssetData.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "DiffUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GitAssetDiff"
DEFINE_LOG_CATEGORY_STATIC(LogGitAssetDiff, Log, All);

IMPLEMENT_MODULE(FGitAssetDiffModule, GitAssetDiff)

namespace
{
	static FString LastGitAssetDiffRevision = TEXT("HEAD");

	void ShowGitAssetDiffMessage(const FText& Message)
	{
		FMessageDialog::Open(EAppMsgType::Ok, Message, LOCTEXT("GitAssetDiffDialogTitle", "Git Asset Diff"));
	}

	FString QuoteCommandLineArgument(const FString& Argument)
	{
		FString Escaped = Argument;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	bool RunGitTextCommand(const FString& RepoRoot, const FString& GitArguments, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
	{
		const FString SafeRepoRoot = FPaths::ConvertRelativePathToFull(RepoRoot);
		const FString Arguments = FString::Printf(
			TEXT("-c %s -C %s %s"),
			*QuoteCommandLineArgument(FString::Printf(TEXT("safe.directory=%s"), *SafeRepoRoot)),
			*QuoteCommandLineArgument(SafeRepoRoot),
			*GitArguments);

		return FPlatformProcess::ExecProcess(TEXT("git"), *Arguments, &OutReturnCode, &OutStdOut, &OutStdErr);
	}

	bool RunShellExportCommand(const FString& Command, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
	{
#if PLATFORM_WINDOWS
		const FString Arguments = FString::Printf(TEXT("/S /C \"%s\""), *Command);
		return FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Arguments, &OutReturnCode, &OutStdOut, &OutStdErr);
#else
		const FString Arguments = FString::Printf(TEXT("-c %s"), *QuoteCommandLineArgument(Command));
		return FPlatformProcess::ExecProcess(TEXT("/bin/sh"), *Arguments, &OutReturnCode, &OutStdOut, &OutStdErr);
#endif
	}

	bool IsSafeGitRevision(const FString& Revision)
	{
		if (Revision.IsEmpty())
		{
			return false;
		}

		for (const TCHAR Character : Revision)
		{
			if (FChar::IsAlnum(Character) ||
				Character == TEXT('.') ||
				Character == TEXT('_') ||
				Character == TEXT('-') ||
				Character == TEXT('/') ||
				Character == TEXT('@') ||
				Character == TEXT('{') ||
				Character == TEXT('}') ||
				Character == TEXT('^') ||
				Character == TEXT('~'))
			{
				continue;
			}
			return false;
		}

		return true;
	}

	FString SanitizeForFilename(FString Value)
	{
		if (Value.IsEmpty())
		{
			return TEXT("Revision");
		}

		for (TCHAR& Character : Value)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-') && Character != TEXT('.'))
			{
				Character = TEXT('_');
			}
		}
		return Value;
	}

	bool PromptForGitRevision(const FAssetData& AssetData, FString& OutRevision)
	{
		bool bAccepted = false;
		FString RevisionText = LastGitAssetDiffRevision;
		TSharedPtr<SEditableTextBox> RevisionTextBox;

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("GitAssetDiffPromptTitle", "Diff Asset Against Git Revision"))
			.ClientSize(FVector2D(500.0f, 150.0f))
			.SizingRule(ESizingRule::Autosized)
			.SupportsMaximize(false)
			.SupportsMinimize(false);

		Window->SetContent(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 12.0f, 12.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("GitAssetDiffPromptAsset", "Git revision or branch for {0}"), FText::FromName(AssetData.AssetName)))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 4.0f)
			[
				SAssignNew(RevisionTextBox, SEditableTextBox)
				.Text(FText::FromString(RevisionText))
				.SelectAllTextWhenFocused(true)
				.OnTextCommitted_Lambda([&RevisionText, &bAccepted, Window](const FText& Text, ETextCommit::Type CommitType)
				{
					if (CommitType == ETextCommit::OnEnter)
					{
						RevisionText = Text.ToString();
						bAccepted = true;
						Window->RequestDestroyWindow();
					}
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 4.0f, 12.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("GitAssetDiffPromptHint", "Examples: main, origin/main, HEAD~1, or a commit hash."))
				.TextStyle(FAppStyle::Get(), "HintText")
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(12.0f, 0.0f, 12.0f, 12.0f)
			[
				SNew(SUniformGridPanel)
				.SlotPadding(FMargin(4.0f, 0.0f))
				+ SUniformGridPanel::Slot(0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("GitAssetDiffPromptOk", "Diff"))
					.OnClicked_Lambda([&RevisionText, &bAccepted, RevisionTextBox, Window]()
					{
						RevisionText = RevisionTextBox.IsValid() ? RevisionTextBox->GetText().ToString() : RevisionText;
						bAccepted = true;
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
				+ SUniformGridPanel::Slot(1, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("GitAssetDiffPromptCancel", "Cancel"))
					.OnClicked_Lambda([Window]()
					{
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
			]
		);

		FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().FindBestParentWindowForDialogs(nullptr));

		RevisionText.TrimStartAndEndInline();
		if (!bAccepted || RevisionText.IsEmpty())
		{
			return false;
		}

		OutRevision = RevisionText;
		LastGitAssetDiffRevision = RevisionText;
		return true;
	}

	bool TryFindGitRootFromPath(const FString& SearchDir, FString& OutRepoRoot)
	{
		FString Candidate = FPaths::ConvertRelativePathToFull(SearchDir);
		FPaths::CollapseRelativeDirectories(Candidate);
		FPaths::NormalizeDirectoryName(Candidate);

		while (!Candidate.IsEmpty())
		{
			const FString DotGitPath = FPaths::Combine(Candidate, TEXT(".git"));
			if (IFileManager::Get().DirectoryExists(*DotGitPath) || IFileManager::Get().FileExists(*DotGitPath))
			{
				OutRepoRoot = Candidate;
				return true;
			}

			const FString Parent = FPaths::GetPath(Candidate);
			if (Parent.Equals(Candidate, ESearchCase::IgnoreCase))
			{
				break;
			}
			Candidate = Parent;
		}

		return false;
	}

	bool ResolveGitRepoRoot(const FString& SearchDir, FString& OutRepoRoot, FText& OutError)
	{
		FString GitSearchDir = SearchDir;
		FString DiscoveredRepoRoot;
		if (TryFindGitRootFromPath(SearchDir, DiscoveredRepoRoot))
		{
			GitSearchDir = DiscoveredRepoRoot;
		}

		FString StdOut;
		FString StdErr;
		int32 ReturnCode = 0;
		if (!RunGitTextCommand(GitSearchDir, TEXT("rev-parse --show-toplevel"), StdOut, StdErr, ReturnCode) || ReturnCode != 0)
		{
			OutError = FText::Format(LOCTEXT("GitAssetDiffRepoRootFailed", "Could not find the Git repository root.\n\n{0}"), FText::FromString(StdErr));
			return false;
		}

		OutRepoRoot = StdOut.TrimStartAndEnd();
		FPaths::CollapseRelativeDirectories(OutRepoRoot);
		FPaths::NormalizeFilename(OutRepoRoot);
		if (FPaths::IsRelative(OutRepoRoot))
		{
			OutRepoRoot = FPaths::ConvertRelativePathToFull(GitSearchDir, OutRepoRoot);
			FPaths::CollapseRelativeDirectories(OutRepoRoot);
		}
		FPaths::NormalizeDirectoryName(OutRepoRoot);
		return true;
	}

	FString GetProjectRootFromProjectFile()
	{
		FString ProjectFilePath = FPaths::GetProjectFilePath();
		if (ProjectFilePath.IsEmpty())
		{
			return FString();
		}

		ProjectFilePath = FPaths::ConvertRelativePathToFull(ProjectFilePath);
		FPaths::CollapseRelativeDirectories(ProjectFilePath);
		FPaths::NormalizeFilename(ProjectFilePath);

		FString ProjectRoot = FPaths::GetPath(ProjectFilePath);
		FPaths::NormalizeDirectoryName(ProjectRoot);
		return ProjectRoot;
	}

	bool TryResolveGamePackageFilename(const FString& PackageName, const FString& Extension, FString& OutFilename)
	{
		if (!PackageName.StartsWith(TEXT("/Game/")))
		{
			return false;
		}

		const FString ProjectRoot = GetProjectRootFromProjectFile();
		if (ProjectRoot.IsEmpty())
		{
			return false;
		}

		const FString PackageExtension = Extension.IsEmpty() ? FPackageName::GetAssetPackageExtension() : Extension;
		FString RelativeContentPath = PackageName.RightChop(6);
		RelativeContentPath += PackageExtension;

		OutFilename = FPaths::Combine(ProjectRoot, TEXT("Content"), RelativeContentPath);
		FPaths::CollapseRelativeDirectories(OutFilename);
		FPaths::NormalizeFilename(OutFilename);
		return IFileManager::Get().FileExists(*OutFilename);
	}

	bool TryUseExistingPackageFilename(
		const FString& CandidateFilename,
		FString& OutCurrentFilename,
		FPackagePath& OutOriginalPackagePath)
	{
		if (CandidateFilename.IsEmpty())
		{
			return false;
		}

		FString AbsoluteFilename = FPaths::ConvertRelativePathToFull(CandidateFilename);
		FPaths::CollapseRelativeDirectories(AbsoluteFilename);
		FPaths::NormalizeFilename(AbsoluteFilename);
		if (!IFileManager::Get().FileExists(*AbsoluteFilename))
		{
			return false;
		}

		OutCurrentFilename = AbsoluteFilename;
		OutOriginalPackagePath = FPackagePath::FromLocalPath(AbsoluteFilename);
		return true;
	}

	bool TryMakeRepoRelativeFilename(
		const FString& AbsoluteFilename,
		const FString& RepoRoot,
		FString& OutRelativeFilename)
	{
		FString NormalizedFilename = AbsoluteFilename;
		FPaths::NormalizeFilename(NormalizedFilename);
		FPaths::CollapseRelativeDirectories(NormalizedFilename);

		FString NormalizedRepoRoot = RepoRoot;
		FPaths::NormalizeDirectoryName(NormalizedRepoRoot);
		FPaths::CollapseRelativeDirectories(NormalizedRepoRoot);

		if (NormalizedRepoRoot.EndsWith(TEXT("/")) || NormalizedRepoRoot.EndsWith(TEXT("\\")))
		{
			NormalizedRepoRoot.LeftChopInline(1, EAllowShrinking::No);
		}

		const FString RepoPrefix = NormalizedRepoRoot + TEXT("/");
		if (!NormalizedFilename.StartsWith(RepoPrefix, ESearchCase::IgnoreCase))
		{
			return false;
		}

		OutRelativeFilename = NormalizedFilename.RightChop(RepoPrefix.Len());
		OutRelativeFilename.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		return !OutRelativeFilename.IsEmpty() &&
			!OutRelativeFilename.StartsWith(TEXT("/")) &&
			!OutRelativeFilename.StartsWith(TEXT("../")) &&
			!OutRelativeFilename.Contains(TEXT("/../")) &&
			!FPaths::IsDrive(OutRelativeFilename.Left(2));
	}

	bool GitObjectExists(const FString& RepoRoot, const FString& Revision, const FString& RelativeFilename)
	{
		FString StdOut;
		FString StdErr;
		int32 ReturnCode = 0;
		const FString Arguments = FString::Printf(
			TEXT("cat-file -e %s"),
			*QuoteCommandLineArgument(FString::Printf(TEXT("%s:%s"), *Revision, *RelativeFilename)));
		return RunGitTextCommand(RepoRoot, Arguments, StdOut, StdErr, ReturnCode) && ReturnCode == 0;
	}

	bool IsUnrealPackageHeaderFilename(const FString& Filename)
	{
		const FString Extension = FPaths::GetExtension(Filename, true);
		return Extension.Equals(FPackageName::GetAssetPackageExtension(), ESearchCase::IgnoreCase) ||
			Extension.Equals(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase);
	}

	bool ReadFilePrefix(const FString& Filename, TArray<uint8>& OutPrefix, const int64 MaxBytes = 4096)
	{
		OutPrefix.Reset();

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const int64 FileSize = PlatformFile.FileSize(*Filename);
		if (FileSize <= 0)
		{
			return false;
		}

		const int32 BytesToRead = static_cast<int32>(FMath::Min<int64>(FileSize, MaxBytes));
		TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*Filename));
		if (!FileHandle)
		{
			return false;
		}

		OutPrefix.SetNumUninitialized(BytesToRead);
		if (!FileHandle->Read(OutPrefix.GetData(), BytesToRead))
		{
			OutPrefix.Reset();
			return false;
		}

		return true;
	}

	bool StartsWithUnrealPackageTag(const TArray<uint8>& Prefix)
	{
		// PACKAGE_FILE_TAG (0x9E2A83C1) is serialized little-endian at the start of .uasset/.umap files.
		return Prefix.Num() >= 4 &&
			Prefix[0] == 0xC1 &&
			Prefix[1] == 0x83 &&
			Prefix[2] == 0x2A &&
			Prefix[3] == 0x9E;
	}

	FString PrefixBytesToText(const TArray<uint8>& Prefix)
	{
		FString Text;
		if (!Prefix.IsEmpty())
		{
			FFileHelper::BufferToString(Text, Prefix.GetData(), Prefix.Num());
		}
		return Text;
	}

	FString MakeSingleLinePreview(FString Text)
	{
		Text.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Text.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		if (Text.Len() > 240)
		{
			Text.LeftInline(240);
			Text += TEXT("...");
		}
		return Text;
	}

	bool ValidateExportedPackageHeader(const FString& ObjectSpec, const FString& OutputFilename, FText& OutError)
	{
		if (!IsUnrealPackageHeaderFilename(OutputFilename))
		{
			return true;
		}

		TArray<uint8> Prefix;
		if (!ReadFilePrefix(OutputFilename, Prefix))
		{
			OutError = FText::Format(LOCTEXT("GitAssetDiffReadExportFailed", "Could not read exported Git object {0}."), FText::FromString(ObjectSpec));
			return false;
		}

		if (StartsWithUnrealPackageTag(Prefix))
		{
			return true;
		}

		const FString PrefixText = PrefixBytesToText(Prefix);
		if (PrefixText.StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")))
		{
			if (PrefixText.Contains(TEXT("<<<<<<<")) && PrefixText.Contains(TEXT("=======")) && PrefixText.Contains(TEXT(">>>>>>>")))
			{
				OutError = FText::Format(
					LOCTEXT(
						"GitAssetDiffLfsConflictExport",
						"Git exported merge-conflict text instead of a binary Unreal asset for:\n{0}\n\nThe selected revision contains an unresolved Git LFS pointer conflict. Resolve that asset in Git, or choose a revision where the asset is a valid .uasset/.umap file.\n\nExport preview:\n{1}"),
					FText::FromString(ObjectSpec),
					FText::FromString(MakeSingleLinePreview(PrefixText)));
			}
			else
			{
				OutError = FText::Format(
					LOCTEXT(
						"GitAssetDiffLfsPointerExport",
						"Git exported a Git LFS pointer instead of a binary Unreal asset for:\n{0}\n\nMake sure Git LFS is installed, filters are configured, and the referenced LFS object exists locally. Then retry the diff.\n\nExport preview:\n{1}"),
					FText::FromString(ObjectSpec),
					FText::FromString(MakeSingleLinePreview(PrefixText)));
			}
			return false;
		}

		OutError = FText::Format(
			LOCTEXT(
				"GitAssetDiffInvalidPackageExport",
				"Git exported data that is not a valid Unreal package for:\n{0}\n\nThe revision file exists, but it does not start with the Unreal package header. The asset may be corrupt in that revision.\n\nExport preview:\n{1}"),
			FText::FromString(ObjectSpec),
			FText::FromString(MakeSingleLinePreview(PrefixText)));
		return false;
	}

	bool ExportGitObjectToFile(const FString& RepoRoot, const FString& Revision, const FString& RelativeFilename, const FString& OutputFilename, FText& OutError)
	{
		const FString SafeRepoRoot = FPaths::ConvertRelativePathToFull(RepoRoot);
		const FString ObjectSpec = FString::Printf(TEXT("%s:%s"), *Revision, *RelativeFilename);
#if PLATFORM_WINDOWS
		const FString Command = FString::Printf(
			TEXT("\"git\" -c %s -C %s cat-file --filters %s > %s"),
			*QuoteCommandLineArgument(FString::Printf(TEXT("safe.directory=%s"), *SafeRepoRoot)),
			*QuoteCommandLineArgument(SafeRepoRoot),
			*QuoteCommandLineArgument(ObjectSpec),
			*QuoteCommandLineArgument(OutputFilename));
#else
		const FString Command = FString::Printf(
			TEXT("git -c %s -C %s cat-file --filters %s > %s"),
			*QuoteCommandLineArgument(FString::Printf(TEXT("safe.directory=%s"), *SafeRepoRoot)),
			*QuoteCommandLineArgument(SafeRepoRoot),
			*QuoteCommandLineArgument(ObjectSpec),
			*QuoteCommandLineArgument(OutputFilename));
#endif

		FString StdOut;
		FString StdErr;
		int32 ReturnCode = 0;
		if (!RunShellExportCommand(Command, StdOut, StdErr, ReturnCode) || ReturnCode != 0)
		{
			OutError = FText::Format(LOCTEXT("GitAssetDiffExportFailed", "Could not export {0} from Git.\n\n{1}"), FText::FromString(ObjectSpec), FText::FromString(StdErr));
			return false;
		}

		if (!IFileManager::Get().FileExists(*OutputFilename) || IFileManager::Get().FileSize(*OutputFilename) <= 0)
		{
			OutError = FText::Format(LOCTEXT("GitAssetDiffExportEmpty", "Git exported an empty or missing file for {0}."), FText::FromString(ObjectSpec));
			return false;
		}

		return ValidateExportedPackageHeader(ObjectSpec, OutputFilename, OutError);
	}

	UObject* FindAssetObjectInPackage(UPackage* Package)
	{
		return Package ? Package->FindAssetInPackage() : nullptr;
	}

	bool ResolveCurrentAssetForDiff(
		const FAssetData& AssetData,
		UObject*& OutCurrentAsset,
		FPackagePath& OutOriginalPackagePath,
		FString& OutCurrentFilename,
		FText& OutError)
	{
		OutCurrentAsset = AssetData.GetAsset();
		if (!OutCurrentAsset)
		{
			OutError = FText::Format(LOCTEXT("GitAssetDiffCurrentAssetLoadFailed", "Could not load the selected asset '{0}'."), FText::FromName(AssetData.PackageName));
			return false;
		}

		UPackage* CurrentPackage = OutCurrentAsset->GetOutermost();
		if (!CurrentPackage)
		{
			OutError = FText::Format(LOCTEXT("GitAssetDiffCurrentPackageMissing", "Selected asset '{0}' has no outer package."), FText::FromName(AssetData.PackageName));
			return false;
		}

		OutOriginalPackagePath = CurrentPackage->GetLoadedPath();
		const FString LoadedPackageFilename = OutOriginalPackagePath.GetLocalFullPath();
		FString PackageExtension = FPaths::GetExtension(LoadedPackageFilename, true);
		if (PackageExtension.IsEmpty())
		{
			PackageExtension = FPackageName::GetAssetPackageExtension();
		}

		FString GamePackageFilename;
		if (TryResolveGamePackageFilename(CurrentPackage->GetName(), PackageExtension, GamePackageFilename))
		{
			OutOriginalPackagePath = FPackagePath::FromLocalPath(GamePackageFilename);
			OutCurrentFilename = GamePackageFilename;
			return true;
		}

		if (TryUseExistingPackageFilename(LoadedPackageFilename, OutCurrentFilename, OutOriginalPackagePath))
		{
			return true;
		}

		FPackagePath MountedPackagePath;
		if (FPackagePath::TryFromPackageName(CurrentPackage->GetFName(), MountedPackagePath) &&
			TryUseExistingPackageFilename(MountedPackagePath.GetLocalFullPath(), OutCurrentFilename, OutOriginalPackagePath))
		{
			return true;
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(CurrentPackage->GetName(), PackageExtension);
		if (TryUseExistingPackageFilename(PackageFilename, OutCurrentFilename, OutOriginalPackagePath))
		{
			return true;
		}

		OutError = FText::Format(
			LOCTEXT("GitAssetDiffCurrentFilenameMissing", "Could not resolve the selected asset to an existing package file.\n\nAssetData package: {0}\nLoaded package: {1}\nLoaded path package: {2}\nLoaded local path: {3}\nProject file: {4}"),
			FText::FromName(AssetData.PackageName),
			FText::FromString(CurrentPackage->GetName()),
			FText::FromString(OutOriginalPackagePath.GetPackageName()),
			FText::FromString(LoadedPackageFilename),
			FText::FromString(FPaths::GetProjectFilePath()));
		return false;
	}
}

void FGitAssetDiffModule::StartupModule()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	RegisterContentBrowserExtensions();
}

void FGitAssetDiffModule::ShutdownModule()
{
	UnregisterContentBrowserExtensions();
}

void FGitAssetDiffModule::RegisterContentBrowserExtensions()
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FGitAssetDiffModule::ExtendAssetContextMenu));
	AssetContextMenuExtenderHandle = Extenders.Last().GetHandle();
}

void FGitAssetDiffModule::UnregisterContentBrowserExtensions()
{
	if (!AssetContextMenuExtenderHandle.IsValid() || !FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
	{
		return Delegate.GetHandle() == AssetContextMenuExtenderHandle;
	});
	AssetContextMenuExtenderHandle.Reset();
}

TSharedRef<FExtender> FGitAssetDiffModule::ExtendAssetContextMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	if (SelectedAssets.Num() == 1 && SelectedAssets[0].IsValid())
	{
		Extender->AddMenuExtension(
			"CommonAssetActions",
			EExtensionHook::After,
			nullptr,
			FMenuExtensionDelegate::CreateRaw(this, &FGitAssetDiffModule::AddAssetDiffMenu, SelectedAssets));
	}
	return Extender;
}

void FGitAssetDiffModule::AddAssetDiffMenu(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets)
{
	if (SelectedAssets.Num() != 1)
	{
		return;
	}

	MenuBuilder.AddSubMenu(
		LOCTEXT("GitAssetDiffSubMenuLabel", "Diff"),
		LOCTEXT("GitAssetDiffSubMenuTooltip", "Diff this asset against a Git revision without switching branches."),
		FNewMenuDelegate::CreateLambda([this, AssetData = SelectedAssets[0]](FMenuBuilder& SubMenuBuilder)
		{
			SubMenuBuilder.AddMenuEntry(
				LOCTEXT("GitAssetDiffMenuLabel", "Against Git Revision..."),
				LOCTEXT("GitAssetDiffMenuTooltip", "Load this asset from another local Git revision and open Unreal's asset diff tool."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Diff"),
				FUIAction(FExecuteAction::CreateRaw(this, &FGitAssetDiffModule::ExecuteDiffAgainstGitRevision, AssetData)));
		}));
}

void FGitAssetDiffModule::ExecuteDiffAgainstGitRevision(FAssetData AssetData)
{
	if (!AssetData.IsValid())
	{
		ShowGitAssetDiffMessage(LOCTEXT("GitAssetDiffInvalidAsset", "The selected asset is not valid."));
		return;
	}

	FString Revision;
	if (!PromptForGitRevision(AssetData, Revision))
	{
		return;
	}

	FText Error;
	if (!DiffAssetAgainstGitRevision(AssetData, Revision, Error))
	{
		ShowGitAssetDiffMessage(Error);
	}
}

bool FGitAssetDiffModule::DiffAssetAgainstGitRevision(
	FAssetData AssetData,
	const FString& Revision,
	FText& OutError,
	FString* OutRepoRoot,
	FString* OutRelativeFilename,
	FString* OutTempAssetFilename)
{
	UObject* CurrentAsset = AssetData.IsValid() ? AssetData.GetAsset() : nullptr;
	UObject* OldAsset = LoadAssetFromGitRevision(AssetData, Revision, OutError, OutRepoRoot, OutRelativeFilename, OutTempAssetFilename);
	if (!OldAsset)
	{
		return false;
	}
	if (!CurrentAsset)
	{
		OutError = LOCTEXT("GitAssetDiffCurrentAssetLoadFailedAfterExport", "Could not load the current asset.");
		return false;
	}

	FRevisionInfo OldRevision;
	OldRevision.Revision = Revision;
	OldRevision.Changelist = -1;
	OldRevision.Date = FDateTime::Now();

	FRevisionInfo CurrentRevision;
	CurrentRevision.Revision = TEXT("");
	CurrentRevision.Changelist = -1;
	CurrentRevision.Date = FDateTime::Now();

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	AssetToolsModule.Get().DiffAssets(OldAsset, CurrentAsset, OldRevision, CurrentRevision);
	return true;
}

UObject* FGitAssetDiffModule::LoadAssetFromGitRevision(
	FAssetData AssetData,
	const FString& Revision,
	FText& OutError,
	FString* OutRepoRoot,
	FString* OutRelativeFilename,
	FString* OutTempAssetFilename)
{
	if (!AssetData.IsValid())
	{
		OutError = LOCTEXT("GitAssetDiffInvalidAsset", "The selected asset is not valid.");
		return nullptr;
	}
	if (!IsSafeGitRevision(Revision))
	{
		OutError = LOCTEXT("GitAssetDiffUnsafeRevision", "The Git revision contains unsupported characters. Use a branch, tag, commit hash, or simple commit-ish such as HEAD~1.");
		return nullptr;
	}

	UObject* CurrentAsset = nullptr;
	FPackagePath OriginalPackagePath;
	FString CurrentFilename;
	if (!ResolveCurrentAssetForDiff(AssetData, CurrentAsset, OriginalPackagePath, CurrentFilename, OutError))
	{
		return nullptr;
	}

	FString RepoRoot;
	if (!ResolveGitRepoRoot(FPaths::GetPath(CurrentFilename), RepoRoot, OutError))
	{
		return nullptr;
	}

	FString RelativeFilename;
	if (!TryMakeRepoRelativeFilename(CurrentFilename, RepoRoot, RelativeFilename))
	{
		OutError = FText::Format(
			LOCTEXT("GitAssetDiffOutsideRepo", "Asset file is not inside the Git repository.\n\nAsset: {0}\nRepository: {1}"),
			FText::FromString(CurrentFilename),
			FText::FromString(RepoRoot));
		return nullptr;
	}
	UE_LOG(
		LogGitAssetDiff,
		Display,
		TEXT("Git asset diff resolved asset '%s' to current='%s', repo='%s', relative='%s', projectFile='%s'"),
		*AssetData.PackageName.ToString(),
		*CurrentFilename,
		*RepoRoot,
		*RelativeFilename,
		*FPaths::GetProjectFilePath());

	if (!GitObjectExists(RepoRoot, Revision, RelativeFilename))
	{
		OutError = FText::Format(LOCTEXT("GitAssetDiffMissingRevision", "Git revision '{0}' does not contain:\n{1}"), FText::FromString(Revision), FText::FromString(RelativeFilename));
		return nullptr;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::DiffDir());

	const FString Prefix = FString::Printf(TEXT("GitAssetDiff-%s-%s-"), *SanitizeForFilename(Revision), *SanitizeForFilename(AssetData.AssetName.ToString()));
	const FString TempAssetFilename = FPaths::ConvertRelativePathToFull(FPaths::CreateTempFilename(*FPaths::DiffDir(), *Prefix, *FPackageName::GetAssetPackageExtension()));
	if (!ExportGitObjectToFile(RepoRoot, Revision, RelativeFilename, TempAssetFilename, OutError))
	{
		return nullptr;
	}

	const FString TempBase = FPaths::ChangeExtension(TempAssetFilename, TEXT(""));
	const FString RelativeBase = FPaths::ChangeExtension(RelativeFilename, TEXT(""));
	const TArray<FString> SidecarExtensions = { TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl") };
	for (const FString& Extension : SidecarExtensions)
	{
		const FString RelativeSidecar = RelativeBase + Extension;
		if (GitObjectExists(RepoRoot, Revision, RelativeSidecar))
		{
			if (!ExportGitObjectToFile(RepoRoot, Revision, RelativeSidecar, TempBase + Extension, OutError))
			{
				return nullptr;
			}
		}
	}

	const FPackagePath TempPackagePath = FPackagePath::FromLocalPath(TempAssetFilename);
	UPackage* RevisionPackage = DiffUtils::LoadPackageForDiff(TempPackagePath, OriginalPackagePath);
	UObject* OldAsset = FindAssetObjectInPackage(RevisionPackage);
	if (!OldAsset)
	{
		OutError = FText::Format(
			LOCTEXT("GitAssetDiffLoadFailed", "Unable to load assets for diff.\n\nRevision asset loaded: {0}\nCurrent asset loaded: {1}"),
			OldAsset ? LOCTEXT("GitAssetDiffYes", "yes") : LOCTEXT("GitAssetDiffNo", "no"),
			CurrentAsset ? LOCTEXT("GitAssetDiffYesCurrent", "yes") : LOCTEXT("GitAssetDiffNoCurrent", "no"));
		return nullptr;
	}
	LastGitAssetDiffRevision = Revision;
	if (OutRepoRoot)
	{
		*OutRepoRoot = RepoRoot;
	}
	if (OutRelativeFilename)
	{
		*OutRelativeFilename = RelativeFilename;
	}
	if (OutTempAssetFilename)
	{
		*OutTempAssetFilename = TempAssetFilename;
	}
	return OldAsset;
}

#undef LOCTEXT_NAMESPACE
