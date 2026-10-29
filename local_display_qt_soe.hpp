#pragma once
#include "edge_gateway/soe_store.hpp"
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

class SoeHistoryView final : public QWidget {
public:
    SoeHistoryView(std::string path, std::string machine, QWidget* parent=nullptr)
        : QWidget(parent),path_(std::move(path)),machine_(std::move(machine)) {
        setObjectName("soeHistory");
        auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
        auto* bar=new QHBoxLayout;
        status_=new QLabel;status_->setObjectName("soeStatus");bar->addWidget(status_,1);
        auto* latest=new QPushButton;latest->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        latest->setToolTip(QString::fromUtf8("最新事件"));latest->setObjectName("soeLatest");bar->addWidget(latest);
        previous_=new QPushButton;previous_->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
        previous_->setToolTip(QString::fromUtf8("上一页"));previous_->setObjectName("soePrevious");bar->addWidget(previous_);
        next_=new QPushButton;next_->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
        next_->setToolTip(QString::fromUtf8("下一页"));next_->setObjectName("soeNext");bar->addWidget(next_);
        for(auto* button:{latest,previous_,next_})button->setFixedSize(40,32);
        layout->addLayout(bar);
        table_=new QTableWidget(0,7);table_->setObjectName("soeTable");
        table_->setHorizontalHeaderLabels({QString::fromUtf8("序号"),QString::fromUtf8("装置时间 (UTC)"),
            QString::fromUtf8("接收时间 (UTC)"),QString::fromUtf8("设备"),QString::fromUtf8("事件"),
            QString::fromUtf8("状态"),QString::fromUtf8("质量")});
        table_->verticalHeader()->hide();table_->verticalHeader()->setDefaultSectionSize(54);
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setAlternatingRowColors(true);table_->setWordWrap(true);table_->setShowGrid(false);layout->addWidget(table_);
        setStyleSheet("QWidget{font-size:14px;color:#303438;background:#FFFFFF;}"
            "QHeaderView::section{background:#F0F3F7;padding:8px;border:0;}"
            "QTableWidget{border:0;alternate-background-color:#F5F7FA;selection-background-color:#E3F2FC;}"
            "QPushButton{border:1px solid #D8E0E4;border-radius:4px;}");
        connect(latest,&QPushButton::clicked,this,[this]{pages_.clear();before_=0;refresh();});
        connect(previous_,&QPushButton::clicked,this,[this]{if(pages_.empty())return;before_=pages_.back();pages_.pop_back();refresh();});
        connect(next_,&QPushButton::clicked,this,[this]{if(!hasMore_||rows_.empty())return;pages_.push_back(before_);before_=rows_.back().id;refresh();});
        auto* timer=new QTimer(this);timer->setInterval(2000);
        connect(timer,&QTimer::timeout,this,[this]{if(isVisible()&&before_==0)refresh();});timer->start();refresh();
    }
    void refresh(){
        try {
            // Never create a database from the display process.
            if(!reader_)reader_.reset(new edge_gateway::SoeStore(path_,true));
            rows_=reader_->read(machine_,before_,51);hasMore_=rows_.size()>50;
            if(hasMore_)rows_.resize(50);
            table_->setRowCount(static_cast<int>(rows_.size()));
            for(std::size_t r=0;r<rows_.size();++r){const auto& e=rows_[r];
                auto stamp=[](std::int64_t ms){return QDateTime::fromMSecsSinceEpoch(ms,Qt::UTC).toString("yyyy-MM-dd\nHH:mm:ss.zzz");};
                const auto quality=!e.valueValid?QString::fromUtf8("DPI 无效"):
                    !e.mapped?QString::fromUtf8("含义待确认"):QString::fromUtf8("有效");
                QStringList values{QString::number(e.id),e.timeValid?stamp(e.deviceTimeMs):QStringLiteral("--"),
                    stamp(e.receivedMs),QString::fromStdString(e.deviceCode),QString::fromStdString(e.name),
                    QString::fromStdString(e.state),quality+QString::fromUtf8(e.timeValid?"\n日期推定":"\n时标无效")};
                const auto details=QString("CA=%1 ASDU=%2 COT=%3 FUN=%4 INF=%5 DPI=%6\nRET=%7 FAN=%8\nCP32=%9\n%10")
                    .arg(e.commonAddress).arg(e.typeId).arg(e.cause).arg(e.functionType).arg(e.informationNumber).arg(e.rawDpi)
                    .arg(e.relativeTimeMs).arg(e.faultNumber).arg(QString::fromStdString(e.rawTimeHex)).arg(QString::fromStdString(e.rawHex));
                for(int c=0;c<values.size();++c){auto* item=new QTableWidgetItem(values[c]);item->setTextAlignment(Qt::AlignCenter);
                    item->setToolTip(values[c]+"\n"+details);table_->setItem(static_cast<int>(r),c,item);}
            }
            status_->setText(rows_.empty()?QString::fromUtf8("暂无装置 SOE 事件"):
                QString::fromUtf8("装置 SOE · 第 %1 页 · %2 条").arg(pages_.size()+1).arg(rows_.size()));
            status_->setToolTip(QString::fromUtf8("CP32 不含日期；日期按接收时刻推定，不代表装置已对时。"));
            previous_->setEnabled(!pages_.empty());next_->setEnabled(hasMore_);
        }catch(const std::exception& ex){
            reader_.reset();rows_.clear();table_->setRowCount(0);hasMore_=false;
            previous_->setEnabled(!pages_.empty());next_->setEnabled(false);
            status_->setText(QString::fromUtf8("SOE 数据不可用"));status_->setToolTip(QString::fromUtf8(ex.what()));
        }
    }
private:
    std::string path_,machine_;
    std::unique_ptr<edge_gateway::SoeStore> reader_;
    std::vector<edge_gateway::Iec103SoeEvent> rows_;
    std::vector<std::int64_t> pages_;
    std::int64_t before_=0;
    bool hasMore_=false;
    QTableWidget* table_=nullptr;
    QLabel* status_=nullptr;
    QPushButton *previous_=nullptr,*next_=nullptr;
};
