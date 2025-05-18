# タスク001: docディレクトリの分析

## 概要
PostgreSQLのdocディレクトリは公式ドキュメントのソースを格納しており、主にSGML形式のファイル群で構成されている。  
ドキュメントはHTML、manページ、PDF、EPUBなど複数の形式にビルド可能で、Makefileによりビルド手順が管理されている。

## doc/src/sgmlディレクトリの構成とビルド方法
- 多数の.sgmlファイルとimages、keywords、refなどのサブディレクトリを含む。
- Makefileでxmllintやxsltproc、pandocなどのツールを用いてSGMLファイルを変換・検証し、各種出力形式を生成。
- 代表的なファイルとしてintro.sgml（序文）、reference.sgml（リファレンス）がある。

## DocBookドキュメントのリンク設計
- README.linksに記載の通り、内部リンクは<xref>や<link>、外部リンクは<ulink>を使用。
- xreflabelを用いることでリンクテキストを制御し、章番号の表示を抑制可能。

## 文字コードの扱い
- README.non-ASCIIにて非ASCII文字はLatin-1文字のHTMLエンティティ表記を推奨。
- PDF出力はLatin-1のみ対応で、非対応文字は警告となる。

## バグ・未実装機能・TODO管理
- doc/KNOWN_BUGS、doc/MISSING_FEATURESはすべてdoc/TODOに統合されている。
- doc/TODO自体は詳細を含まず、PostgreSQLのWikiページ（https://wiki.postgresql.org/wiki/Todo）で管理されている。

## doc/src/Makefileの役割
- doc/src/MakefileはsgmlディレクトリのMakefileを呼び出す簡易的なもの。

---

以上、docディレクトリのドキュメント構成と管理方針の概要を整理した。
