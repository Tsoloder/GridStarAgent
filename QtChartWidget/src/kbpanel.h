#ifndef GS_KBPANEL_H
#define GS_KBPANEL_H

#include <QList>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLayoutItem;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QVBoxLayout;
QT_END_NAMESPACE

namespace gs {

class ElidedLabel;

// 归档树的一行（style.css .kb-tree-row）：twisty + 名称 + 数量。
// 行本身是可点的（Enter / Space 等价左键），同时是资料行的落点：
// 拖着库里的一份资料过来时高亮并接受 drop，交给面板发 documentMoveRequested。
class KbTreeRow : public QWidget
{
    Q_OBJECT
public:
    // key：all / root / 文件夹 id；depth 从 0 起（库根为 0，文件夹按 depth+1 缩进）
    KbTreeRow(const QString &key, const QString &label, int count, int depth, bool hasChildren,
              bool expanded, QWidget *parent = nullptr);

    QString folderKey() const { return m_key; }
    void setActive(bool active);
    void setExpanded(bool expanded);
    void setDropTarget(bool on);
    bool isDropTarget() const { return m_dropTarget; }

signals:
    void activated(const QString &key);
    void twistyToggled(const QString &key);
    void documentDropped(const QString &key, const QString &docId);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void updateTwisty();

    QString m_key;
    QString m_label;
    int m_count = 0;
    int m_depth = 0;
    bool m_hasChildren = false;
    bool m_expanded = false;
    bool m_active = false;
    bool m_dropTarget = false;
    QLabel *m_twisty = nullptr;
    ElidedLabel *m_name = nullptr;
    QLabel *m_countLabel = nullptr;
};

// 资料清单的一行（style.css .kb-doc）：拖动把手 + 名称 + 元信息 + 重建 / 删除。
// 拖拽用自定义 mime（application/x-gridstar-kb-document，附 doc_id），
// 失败态的行不可拖（与 app.js renderKbDocs 的 row.draggable = !failed 同义）。
class KbDocRow : public QWidget
{
    Q_OBJECT
public:
    explicit KbDocRow(const QVariantMap &doc, QWidget *parent = nullptr);

    QString docId() const { return m_docId; }
    bool isFailed() const { return m_failed; }
    void setDragging(bool dragging);
    void setActionsEnabled(bool enabled);

signals:
    void deleteRequested(const QString &docId);
    void reindexRequested(const QString &docId);
    // 拖动结束（无论有没有落地）：面板据此清掉所有落点高亮
    void dragFinished();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    bool pressOnButton(const QPoint &pos) const;
    void startDrag();

    QString m_docId;
    bool m_failed = false;
    bool m_pressed = false;
    QPoint m_pressPos;
    QLabel *m_grip = nullptr;
    ElidedLabel *m_name = nullptr;
    ElidedLabel *m_meta = nullptr;
    QPushButton *m_reindex = nullptr;
    QPushButton *m_delete = nullptr;
};

// 设置中心「知识库」页（webui #panel-kb / app.js kb* 一组函数）。
//
// 结构（与 index.html 同序）：
//   状态条
//   → 库栏（选择 / 新建 / 重命名 / 删除 / 刷新）
//   → 添加文件 · 导入目录
//   → 库卡片（文档 / 分块 / 参数 / 索引体积 / 失败）
//   → 左右分栏：归档树（文件夹增删改、拖拽归档） | 资料清单（重建索引 / 删除）
//   → 检索试跑（不经过模型，直接看检索返回）
//
// 库不做网络：本类只渲染数据、发意图信号。文件解析 / 上传 / 索引全部由宿主完成。
// 面板自身是纵向滚动容器（style.css .kb-panel{overflow-y:auto}），外层不要再套滚动区。
class KbPanel : public QWidget
{
    Q_OBJECT
public:
    explicit KbPanel(QWidget *parent = nullptr);

    void enterTab();
    void leaveTab();

    // ---- 宿主回填 ----
    // GET /kb/libraries：{libraries:[{id, name, documents, chunks}]}
    void setLibraries(const QVariantList &libraries);
    // GET /kb/status：{available, error, documents, chunks, params, ...}
    void setStatus(const QVariantMap &status);
    // GET /kb/folders?library_id=...：{folders:[{id, name, parent_id, depth}], tree:{...}}
    void setFolders(const QVariantList &folders, const QVariantMap &tree);
    // GET /kb/documents?library_id=&folder=...
    void setDocuments(const QVariantList &documents, int total);
    // GET /kb/search?q=...&top_k=6：{params:[...], hits:[...]}
    void setSearchResult(const QVariantMap &result);
    void setBusy(bool busy);
    void setStatusText(const QString &text);

    // 当前在面板里选中的知识库 id（宿主拉取时用作 library_id；空串表示后端默认库）
    QString currentLibrary() const;

signals:
    // 宿主收到后拉 /kb/libraries + /kb/status + /kb/folders + /kb/documents
    void refreshRequested(const QString &libraryId);
    // 面板里切了知识库：宿主按新库重拉 folders / documents / status
    void libraryChanged(const QString &libraryId);
    // 面板里切了归档目录（用户点树，或切库后回到「全部资料」）：
    // folderId 与树上的 key 一致（all / root / 真实文件夹 id），
    // 宿主据此拉 GET /kb/documents?library_id=&folder= 再 setDocuments 回填
    void folderChanged(const QString &libraryId, const QString &folderId);

    void libraryCreateRequested(const QString &name);
    void libraryRenameRequested(const QString &libraryId, const QString &name);
    void libraryDeleteRequested(const QString &libraryId);

    // 宿主负责逐份 POST /kb/import（含上传进度），回填后调 refreshRequested
    void filesImportRequested(const QStringList &paths, const QString &libraryId,
                              const QString &folderId);
    void dirImportRequested(const QString &libraryId, const QString &dirPath);
    // 目录导入带上目标文件夹（落在当前选中的归档目录）：webui 的目录导入只带库，
    // 落到库根；实际使用里导入一个子目录往往就是想归到当前目录，所以拆出一个新信号。
    // folderId 为空串表示库根。旧的 dirImportRequested 保留但不再 emit（宿主改接这个）
    void dirImportToFolderRequested(const QString &libraryId, const QString &dirPath,
                                    const QString &folderId);

    void documentDeleteRequested(const QString &docId);
    void documentReindexRequested(const QString &docId);
    // 拖拽归档：folderId 为空串表示移回库根
    void documentMoveRequested(const QString &docId, const QString &folderId);

    void folderCreateRequested(const QString &libraryId, const QString &parentId,
                               const QString &name);
    void folderRenameRequested(const QString &folderId, const QString &name);
    void folderDeleteRequested(const QString &folderId);

    void searchRequested(const QString &libraryId, const QString &query);

protected:
    void showEvent(QShowEvent *event) override;
    // 皮肤 / 缩放变了（全局样式表重设会发 StyleChange）：QSS 由 theme.cpp 重建，
    // 这里只需把怕缩放的内联尺寸与行高重算一遍
    bool event(QEvent *event) override;

private:
    // ---- 构建 ----
    void buildUi();
    QWidget *buildLibBar();
    QWidget *buildActions();
    QWidget *buildCards();
    QWidget *buildBody();
    QWidget *buildDocsSection(QWidget *parent);
    QWidget *buildSearchSection();
    // 重排（非样式）：样式表在 theme.cpp，这里只按当前缩放重算内联尺寸并重建行高。
    // 皮肤 / 缩放变化后重铺（showEvent / StyleChange 里比对 styleKey）。
    void applyStyle();
    QString styleKey() const;

    // ---- 渲染 ----
    void rebuildLibraryCombo();
    void rebuildCards();
    void rebuildTree();
    void rebuildDocs();
    void rebuildSearchResult();
    void renderStatus();
    void syncEnabled();

    // ---- 取值 ----
    QString libraryName(const QString &id) const;
    QString folderLabel(const QString &key) const;
    QString folderPathText(const QString &key) const;
    // 导入目标：「全部资料」「库根」都落到库根（空串），其余落到选中文件夹
    QString targetFolderId() const;
    bool isRealFolder(const QString &key) const;
    int folderTotal(const QString &folderId) const;
    QList<QVariantMap> visibleDocuments() const;
    QString documentName(const QString &docId) const;
    // 状态条口径（app.js renderKb）：返回是否可用
    bool statusReady() const;
    bool statusFts() const;

    // ---- 交互 ----
    void pickLibrary(const QString &id);
    void selectFolder(const QString &key);
    void toggleFolder(const QString &key);
    void createLibrary();
    void renameLibrary();
    void deleteLibrary();
    void createFolder();
    void renameFolder();
    void deleteFolder();
    void pickFiles();
    void importDirectory();
    void runSearch();
    void clearDropHints();
    void resetFolderState();
    // 换库时把上一份归档 / 资料清掉：宿主回填之前不要显示另一个库的内容
    void clearLibraryData();

    void requestDocumentDelete(const QString &docId);

    void clearLayout(QVBoxLayout *layout);
    void clearLayout(QHBoxLayout *layout);
    KbTreeRow *addTreeRow(const QString &key, const QString &label, int count, int depth,
                          bool hasChildren);
    QFrame *addCard(const QString &label, const QString &value, const QString &note, bool warn);

    // ---- 状态（对应 app.js kbState） ----
    QString m_library;                 // 面板正在看哪个库（panelLibrary）
    QVariantList m_libraries;
    QVariantMap m_status;
    QVariantList m_folders;
    QVariantMap m_tree;
    QVariantList m_docs;
    int m_docsTotal = 0;
    QString m_currentFolder = QStringLiteral("all");
    QSet<QString> m_expanded;          // 展开中的文件夹（含固定的 "root"）
    QSet<QString> m_knownFolders;      // 见过的文件夹：只给新出现的默认展开
    QVariantMap m_search;
    QString m_searchNotice;            // 「先输一个检索词。」这类本地提示
    QString m_statusOverride;
    bool m_searchPending = false;
    bool m_pendingCreate = false;      // 刚发过 libraryCreateRequested，等新库 id 回来
    bool m_busy = false;
    QString m_styleKey;

    // ---- 控件 ----
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_root = nullptr;
    QLabel *m_statusLabel = nullptr;
    QComboBox *m_libCombo = nullptr;
    QPushButton *m_libNew = nullptr;
    QPushButton *m_libRename = nullptr;
    QPushButton *m_libDelete = nullptr;
    QPushButton *m_libRefresh = nullptr;
    QPushButton *m_pickButton = nullptr;
    QLineEdit *m_dirInput = nullptr;
    QPushButton *m_importDirButton = nullptr;
    QWidget *m_cardsBox = nullptr;
    QHBoxLayout *m_cardsLayout = nullptr;
    QFrame *m_treeColumn = nullptr;
    QScrollArea *m_treeScroll = nullptr;
    QWidget *m_treeInner = nullptr;
    QVBoxLayout *m_treeLayout = nullptr;
    QPushButton *m_folderNew = nullptr;
    QPushButton *m_folderRename = nullptr;
    QPushButton *m_folderDelete = nullptr;
    QLabel *m_docTitle = nullptr;
    QLabel *m_docCount = nullptr;
    QScrollArea *m_docsScroll = nullptr;
    QWidget *m_docsInner = nullptr;
    QVBoxLayout *m_docsLayout = nullptr;
    QLineEdit *m_query = nullptr;
    QPushButton *m_searchButton = nullptr;
    QVBoxLayout *m_searchLayout = nullptr;

    QList<KbTreeRow *> m_treeRows;
    QList<KbDocRow *> m_docRows;
};

} // namespace gs

#endif // GS_KBPANEL_H